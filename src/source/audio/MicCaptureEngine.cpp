// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/audio/MicCaptureEngine.h"

#include "core/model/Protocol.h"
#include "source/audio/OpusAudioCodec.h"

#include <QLoggingCategory>

#include <algorithm>
#include <exception>
#include <utility>

namespace dish::source::audio {

namespace {
Q_LOGGING_CATEGORY(lcDishAudio, "dish.audio")
} // namespace

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
    } catch (const std::exception& e) {
        // A teardown IO failure has nowhere to go: the devices are closing with
        // the process either way, and a throwing destructor terminates. It is
        // still logged, so a repeating one is visible.
        qCWarning(lcDishAudio) << "mic capture teardown failed:" << e.what();
    } catch (...) { qCWarning(lcDishAudio) << "mic capture teardown failed"; }
}

// The whole per-window pipeline, on the gateway's audio thread: window, re-check enabled, encode,
// seq++, send.
//
// The enabled re-check inside the window emit, and not only per chunk, is the half of the privacy
// invariant that bounds a mute landing mid-close to one frame.
void MicCaptureEngine::WindowPublisher::operator()(const std::int16_t* samples,
                                                   std::size_t count) const {
    if (!session_->enabled.load(std::memory_order_relaxed)) { return; }
    session_->windower.feed(samples, count, [this](const std::int16_t* pcm, std::size_t frames) {
        sendWindow(pcm, frames);
    });
}

void MicCaptureEngine::WindowPublisher::sendWindow(const std::int16_t* pcm,
                                                   std::size_t frames) const {
    if (!session_->enabled.load(std::memory_order_relaxed)) { return; }
    std::uint8_t packet[proto::kAudioWireMaxOpusBytes];
    const std::size_t bytes = session_->encoder->encode(pcm, frames, packet, sizeof(packet));
    // The window happened whether or not it encoded, so the seq slot is spent either way; a
    // failed encode becomes the receiver's concealment, not a shortened stream.
    const std::uint16_t seq = session_->seq.fetch_add(1, std::memory_order_relaxed);
    if (bytes == 0) { return; }
    session_->send(seq, packet, bytes);
}

// A slot that moved endpoints, or fell out of eligibility (a mute among the causes), loses its
// capture here.
void MicCaptureEngine::closeDeparted(const std::vector<MicCaptureTarget>& targets) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const MicCaptureTarget* wanted = nullptr;
        for (const auto& t : targets) {
            if (t.slotId == it->first) {
                wanted = &t;
                break;
            }
        }
        if (wanted == nullptr || wanted->captureDeviceName != it->second.deviceName) {
            close(it->first, it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void MicCaptureEngine::closeOnDevicesAbsent(const std::vector<std::string>& presentDevices) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const bool present = std::find(presentDevices.begin(), presentDevices.end(),
                                       it->second.deviceName) != presentDevices.end();
        if (present) {
            ++it;
            continue;
        }
        close(it->first, it->second);
        it = entries_.erase(it);
    }
}

void MicCaptureEngine::openMissing(const std::vector<MicCaptureTarget>& targets) {
    for (const auto& t : targets) {
        if (t.slotId.empty() || t.captureDeviceName.empty() || !t.send) { continue; }
        if (entries_.find(t.slotId) != entries_.end()) { continue; }

        auto session = std::make_shared<Session>();
        session->encoder = encoderFactory_();
        if (session->encoder == nullptr) { continue; } // no codec, no capture
        session->send = t.send;
        const auto resume = nextSeqBySlot_.find(t.slotId);
        const std::uint16_t resumedSeq = resume != nextSeqBySlot_.end() ? resume->second : 0;
        session->seq.store(resumedSeq, std::memory_order_relaxed);

        Entry entry;
        entry.deviceName = t.captureDeviceName;
        entry.session = session;
        entry.handle = gateway_->openCapture(t.captureDeviceName, WindowPublisher{session});
        if (entry.handle == kNoAudioDevice) { continue; }
        entries_.emplace(t.slotId, std::move(entry));
    }
}

void MicCaptureEngine::reconcile(const std::vector<MicCaptureTarget>& targets) {
    if (gateway_ == nullptr) { return; }
    // Narrowing before widening, so a slot that moved endpoints is never captured from two
    // devices at once.
    closeDeparted(targets);
    openMissing(targets);
}

bool MicCaptureEngine::capturingFor(const std::string& slotId) const {
    return entries_.find(slotId) != entries_.end();
}

void MicCaptureEngine::close(const std::string& slotId, Entry& entry) {
    // Disable BEFORE the close: the close joins the callback, but the flag is
    // what drops a window already captured when the decision landed.
    if (entry.session != nullptr) {
        entry.session->enabled.store(false, std::memory_order_relaxed);
    }
    if (entry.handle != kNoAudioDevice) { gateway_->closeCapture(entry.handle); }
    // The close joined the callback, so the count is at rest; the slot's next
    // capture resumes from it.
    if (entry.session != nullptr) {
        nextSeqBySlot_[slotId] = entry.session->seq.load(std::memory_order_relaxed);
    }
}

} // namespace dish::source::audio
