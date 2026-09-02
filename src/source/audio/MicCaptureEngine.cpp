// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/audio/MicCaptureEngine.h"

#include "core/model/Protocol.h"
#include "source/audio/OpusAudioCodec.h"

#include <utility>

namespace dish::source::audio {

MicCaptureEngine::MicCaptureEngine(AudioDeviceGateway* gateway, EncoderFactory encoderFactory)
    : gateway_(gateway), encoderFactory_(std::move(encoderFactory)) {
    if (!encoderFactory_) {
        encoderFactory_ = [] {
            return std::unique_ptr<dish::audio::IAudioEncoder>(
                dish::audio::OpusStreamEncoder::create(dish::audio::Stream::Mic));
        };
    }
}

MicCaptureEngine::~MicCaptureEngine() {
    try {
        reconcile({});
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // A teardown IO failure has nowhere to go; the devices are closing
        // with the process either way, and a throwing destructor terminates.
    }
}

void MicCaptureEngine::reconcile(const std::vector<MicCaptureTarget>& targets) {
    if (gateway_ == nullptr) { return; }

    // Close first — narrowing before widening, so a slot that moved endpoints
    // is never captured from two devices at once, and a slot that fell out of
    // eligibility (a mute among the causes) is closed before anything new
    // starts.
    for (auto it = entries_.begin(); it != entries_.end();) {
        const MicCaptureTarget* wanted = nullptr;
        for (const auto& t : targets) {
            if (t.slotId == it->first) {
                wanted = &t;
                break;
            }
        }
        if (wanted == nullptr || wanted->captureDeviceName != it->second.deviceName) {
            close(it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& t : targets) {
        if (t.slotId.empty() || t.captureDeviceName.empty() || !t.send) { continue; }
        if (entries_.find(t.slotId) != entries_.end()) { continue; }
        auto session = std::make_shared<Session>();
        session->encoder = encoderFactory_();
        if (session->encoder == nullptr) { continue; } // no codec, no capture
        session->send = t.send;

        // The whole per-window pipeline lives in this callback, on the
        // gateway's audio thread: window -> re-check enabled -> encode ->
        // seq++ -> send. The enabled re-check inside the window emit (not just
        // per chunk) is the half of the privacy invariant that bounds a mute
        // landing mid-close to one frame.
        auto onSamples = [session](const std::int16_t* samples, std::size_t count) {
            if (!session->enabled.load(std::memory_order_relaxed)) { return; }
            session->windower.feed(
                samples, count, [&session](const std::int16_t* pcm, std::size_t frames) {
                    if (!session->enabled.load(std::memory_order_relaxed)) { return; }
                    std::uint8_t packet[proto::kAudioWireMaxOpusBytes];
                    const std::size_t bytes =
                        session->encoder->encode(pcm, frames, packet, sizeof(packet));
                    // The window happened whether or not it encoded, so the
                    // seq slot is spent either way; a failed encode becomes
                    // the receiver's concealment, not a shortened stream.
                    const std::uint16_t seq = session->seq++;
                    if (bytes == 0) { return; }
                    session->send(seq, packet, bytes);
                });
        };

        Entry entry;
        entry.deviceName = t.captureDeviceName;
        entry.session = session;
        entry.handle = gateway_->openCapture(t.captureDeviceName, onSamples);
        if (entry.handle == kNoAudioDevice) { continue; }
        entries_.emplace(t.slotId, std::move(entry));
    }
}

bool MicCaptureEngine::capturingFor(const std::string& slotId) const {
    return entries_.find(slotId) != entries_.end();
}

void MicCaptureEngine::close(Entry& entry) {
    // Disable BEFORE the close: the close joins the callback, but the flag is
    // what drops a window already captured when the decision landed.
    if (entry.session != nullptr) {
        entry.session->enabled.store(false, std::memory_order_relaxed);
    }
    if (entry.handle != kNoAudioDevice) { gateway_->closeCapture(entry.handle); }
}

} // namespace dish::source::audio
