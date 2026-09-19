// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/audio/SpeakerPlayoutEngine.h"

#include "core/audio/AudioEnginePolicy.h"
#include "core/model/Protocol.h"
#include "source/audio/OpusAudioCodec.h"

#include <array>
#include <utility>

namespace dish::source::audio {

namespace {

// One decoded 20 ms stereo window, interleaved. Both lanes are stereo on the
// wire.
static_assert(proto::kAudioHapticChannels == proto::kAudioSpeakerChannels,
              "both lanes decode to one window shape");
constexpr std::size_t kWindowSamples = static_cast<std::size_t>(proto::kAudioFrameSamples) *
                                       static_cast<std::size_t>(proto::kAudioSpeakerChannels);

// Where a lane's stereo pair sits in the endpoint's channel order: speaker
// first, then haptics, which is the DualSense's own layout.
constexpr int laneOffset(PlayoutLane lane) {
    return lane == PlayoutLane::Speaker ? 0 : proto::kAudioSpeakerChannels;
}

// One window's byte size at the device's channel count, for the cushion math.
constexpr std::size_t frameBytesFor(int channels) {
    return static_cast<std::size_t>(proto::kAudioFrameSamples) *
           static_cast<std::size_t>(channels) * sizeof(std::int16_t);
}

} // namespace

SpeakerPlayoutEngine::SpeakerPlayoutEngine(AudioDeviceGateway* gateway,
                                           DecoderFactory decoderFactory)
    : gateway_(gateway), decoderFactory_(std::move(decoderFactory)) {
    if (!decoderFactory_) {
        // One decoder shape serves both lanes: the haptic stream is the
        // speaker's format on the wire.
        decoderFactory_ = [] {
            return std::unique_ptr<dish::audio::IAudioDecoder>(
                dish::audio::OpusStreamDecoder::create(dish::audio::Stream::Speaker));
        };
    }
}

SpeakerPlayoutEngine::~SpeakerPlayoutEngine() {
    try {
        reconcile({});
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Same rule as the capture engine: nowhere to report a teardown
        // failure, and the handles die with the process anyway.
    }
}

void SpeakerPlayoutEngine::reconcile(const std::vector<SpeakerVoiceTarget>& targets) {
    if (gateway_ == nullptr) { return; }
    std::lock_guard<std::mutex> lock(mtx_);

    for (auto it = voices_.begin(); it != voices_.end();) {
        const SpeakerVoiceTarget* wanted = nullptr;
        for (const auto& t : targets) {
            if (t.connectionId == std::get<0>(it->first) &&
                t.controllerIndex == std::get<1>(it->first) && t.lane == std::get<2>(it->first)) {
                wanted = &t;
                break;
            }
        }
        // A changed channel count is a changed device (the endpoint was
        // re-enumerated), so the stream reopens at the new width.
        if (wanted == nullptr || wanted->playbackDeviceName != it->second->deviceName ||
            wanted->slotId != it->second->slotId ||
            wanted->deviceChannels != it->second->channels) {
            closeLocked(*it->second);
            it = voices_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& t : targets) {
        if (t.connectionId.empty() || t.playbackDeviceName.empty()) { continue; }
        // A lane needs at least its own pair to exist at the offset it writes.
        if (t.deviceChannels < laneOffset(t.lane) + proto::kAudioSpeakerChannels) { continue; }
        const VoiceKey key{t.connectionId, t.controllerIndex, t.lane};
        if (voices_.find(key) != voices_.end()) { continue; }
        auto voice = std::make_unique<Voice>();
        voice->slotId = t.slotId;
        voice->deviceName = t.playbackDeviceName;
        voice->lane = t.lane;
        voice->channels = t.deviceChannels;
        voice->decoder = decoderFactory_();
        if (voice->decoder == nullptr) { continue; } // no codec, no voice
        voice->handle = gateway_->openPlayback(t.playbackDeviceName, t.deviceChannels);
        if (voice->handle == kNoAudioDevice) { continue; }
        voices_.emplace(key, std::move(voice));
    }
}

void SpeakerPlayoutEngine::deliver(const std::string& connectionId, int controllerIndex,
                                   PlayoutLane lane, std::uint16_t seq, const std::uint8_t* opus,
                                   std::size_t opusLen) {
    if (opus == nullptr || opusLen == 0) { return; }
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = voices_.find(VoiceKey{connectionId, controllerIndex, lane});
    if (it == voices_.end()) { return; }
    Voice& voice = *it->second;
    const std::size_t frameBytes = frameBytesFor(voice.channels);
    const std::size_t deviceWindow = static_cast<std::size_t>(proto::kAudioFrameSamples) *
                                     static_cast<std::size_t>(voice.channels);

    const auto result = voice.window.push(seq, opus, opusLen);
    std::array<std::int16_t, kWindowSamples> pcm{};
    for (int i = 0; i < result.count; i++) {
        const auto& event = result.events[i];
        std::size_t frames = 0;
        if (event.kind == dish::audio::AudioJitterWindow::Event::Kind::Packet) {
            frames = voice.decoder->decode(event.data, event.len, pcm.data(),
                                           static_cast<std::size_t>(proto::kAudioFrameSamples));
        } else {
            // The gap path takes FEC unconditionally: whether the carrier
            // packet really holds a redundant copy is an encoder decision this
            // end cannot see, and the decoder degrades to plain concealment on
            // its own — a null carrier included.
            frames = voice.decoder->decodeFec(event.fecCarrier, event.fecCarrierLen, pcm.data(),
                                              static_cast<std::size_t>(proto::kAudioFrameSamples));
        }
        if (frames == 0) { continue; }

        // A queue that drained mid-stream (a suppressed-silence stretch) gets
        // its cushion back as silence ahead of the resumed audio.
        const int refill = dish::audio::playoutRefillFrames(
            voice.started, gateway_->queuedPlaybackBytes(voice.handle));
        if (refill > 0) {
            const std::vector<std::int16_t> silence(deviceWindow, 0);
            for (int f = 0; f < refill; f++) {
                gateway_->queuePlayback(voice.handle, silence.data(), silence.size());
            }
        }

        // The wire's stereo into the lane's pair of the device's channels;
        // a stereo device takes the window as it is.
        const std::int16_t* out = pcm.data();
        std::size_t outCount = frames * static_cast<std::size_t>(proto::kAudioSpeakerChannels);
        if (voice.channels != proto::kAudioSpeakerChannels) {
            const auto stride = static_cast<std::size_t>(voice.channels);
            const auto off = static_cast<std::size_t>(laneOffset(voice.lane));
            voice.spread.assign(frames * stride, 0);
            for (std::size_t f = 0; f < frames; f++) {
                voice.spread[f * stride + off] = pcm[f * 2];
                voice.spread[f * stride + off + 1] = pcm[f * 2 + 1];
            }
            out = voice.spread.data();
            outCount = voice.spread.size();
        }
        gateway_->queuePlayback(voice.handle, out, outCount);
        if (!voice.started &&
            gateway_->queuedPlaybackBytes(voice.handle) >=
                static_cast<std::size_t>(dish::audio::kPlayoutStartThresholdFrames) * frameBytes) {
            gateway_->resumePlayback(voice.handle);
            voice.started = true;
        }
    }
}

bool SpeakerPlayoutEngine::playingFor(const std::string& slotId, PlayoutLane lane) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [key, voice] : voices_) {
        if (voice->slotId == slotId && voice->lane == lane) { return true; }
    }
    return false;
}

void SpeakerPlayoutEngine::closeLocked(Voice& voice) {
    if (voice.handle != kNoAudioDevice) { gateway_->closePlayback(voice.handle); }
}

} // namespace dish::source::audio
