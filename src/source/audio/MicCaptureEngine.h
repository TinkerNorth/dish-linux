// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The microphone capture pipeline: one open capture device per eligible slot
// (the pad's OWN headset mic endpoint — the matcher never routes two slots to
// one endpoint, so per-slot IS per-device), windowed to exact 20 ms frames,
// Opus-encoded and handed to the slot's MSG_MIC_AUDIO sender.
//
// THE PRIVACY INVARIANT. Muted, toggled off, unrouted, unstreaming or
// unwelcome at the host means ZERO MSG_MIC_AUDIO packets AND a CLOSED capture
// device — never silence sent in their place. It is enforced twice:
//
//  - structurally: eligibility (core/audio/AudioEnginePolicy.h) decides the
//    target list BEFORE it reaches reconcile(), and a slot absent from the
//    list has its device closed — closeCapture is a join, so after reconcile
//    returns no callback is executing;
//  - per window: the session's `enabled` flag is cleared FIRST, before the
//    close is even requested, and the audio-thread callback re-checks it per
//    delivery, so a window captured while the close is in flight is dropped
//    rather than shipped. That bounds mute latency to one 20 ms frame.
//
// `seq` is caller-owned per stream and advances once per captured window,
// INCLUDING windows whose encode failed: that 20 ms really happened, so the
// receiver conceals it rather than playing the stream short and drifting
// against the pad's clock. (A DTX silence frame is not a failure — it is a
// 1-byte packet and it is sent.)

#pragma once

#include "core/audio/AudioCodec.h"
#include "core/audio/AudioEnginePolicy.h"
#include "source/audio/AudioDeviceGateway.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dish::source::audio {

// One eligible slot: where to capture from and how to send. The sender runs on
// the gateway's audio thread and must be thread-safe (WifiConnection's send
// path is, by the same contract as the SDL input thread's).
struct MicCaptureTarget {
    std::string slotId;
    std::string captureDeviceName;
    std::function<bool(std::uint16_t seq, const std::uint8_t* opus, std::size_t len)> send;
};

class MicCaptureEngine {
  public:
    using EncoderFactory = std::function<std::unique_ptr<dish::audio::IAudioEncoder>()>;

    // The default factory builds the wire's mic encoder (Opus VOIP mono, DTX);
    // tests inject a fake to make encode failures and byte-exact sends
    // observable.
    explicit MicCaptureEngine(AudioDeviceGateway* gateway, EncoderFactory encoderFactory = {});
    ~MicCaptureEngine();

    MicCaptureEngine(const MicCaptureEngine&) = delete;
    MicCaptureEngine& operator=(const MicCaptureEngine&) = delete;
    MicCaptureEngine(MicCaptureEngine&&) = delete;
    MicCaptureEngine& operator=(MicCaptureEngine&&) = delete;

    // Converge on exactly `targets`: open what is missing, close what is not
    // wanted (or whose device moved), leave the rest running. Main thread.
    // An open the device refuses is dropped silently; the next reconcile —
    // routes and claims re-trigger it — is the retry.
    void reconcile(const std::vector<MicCaptureTarget>& targets);

    // Whether a capture device is open for the slot right now (UI/tests).
    bool capturingFor(const std::string& slotId) const;

  private:
    // Shared with the audio-thread callback via shared_ptr, so a close racing
    // a callback can never leave the callback on freed state.
    struct Session {
        std::atomic<bool> enabled{true};
        dish::audio::FrameWindower windower;
        std::unique_ptr<dish::audio::IAudioEncoder> encoder;
        std::uint16_t seq = 0; // audio-thread only after start
        std::function<bool(std::uint16_t, const std::uint8_t*, std::size_t)> send;
    };
    struct Entry {
        int handle = kNoAudioDevice;
        std::string deviceName;
        std::shared_ptr<Session> session;
    };

    void close(Entry& entry);

    AudioDeviceGateway* gateway_;
    EncoderFactory encoderFactory_;
    std::map<std::string, Entry> entries_; // by slotId; main thread only
};

} // namespace dish::source::audio
