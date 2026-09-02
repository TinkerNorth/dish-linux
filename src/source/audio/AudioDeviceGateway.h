// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The IO boundary the controller-audio engines talk to, so the capture/playout
// policy and plumbing are testable against a fake with no SDL and no sound
// card — the same seam convention as UsbDeviceGateway over the raw-HID stack.
// Production swaps in SdlAudioGateway.
//
// IO only, no domain state: which device to open, when, and what to do with
// the samples are the engines' decisions.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dish::source::audio {

// Invalid/failed device handle. Real handles are positive.
inline constexpr int kNoAudioDevice = 0;

class AudioDeviceGateway {
  public:
    virtual ~AudioDeviceGateway() = default;

    // The device names present right now, per direction. Names are the open
    // keys for the two open calls below, verbatim. Empty when the audio
    // subsystem is unavailable (headless CI), which reads as "no routes" and
    // keeps everything downstream conservatively off.
    virtual std::vector<std::string> captureDeviceNames() = 0;
    virtual std::vector<std::string> playbackDeviceNames() = 0;

    // Open one capture device at the wire's mic format (48 kHz mono S16).
    // `onSamples` fires on the gateway's audio thread with however many mono
    // samples the platform's period produced — the caller windows them
    // (core/audio/AudioEnginePolicy.h FrameWindower). Returns kNoAudioDevice
    // when the device refused. The device starts capturing immediately.
    virtual int openCapture(const std::string& deviceName,
                            std::function<void(const std::int16_t*, std::size_t)> onSamples) = 0;

    // Stop and close a capture handle. MUST NOT return while a callback is
    // still executing — the privacy invariant leans on "closed means no more
    // samples", so this is a join, not a request. Idempotent for unknown
    // handles.
    virtual void closeCapture(int handle) = 0;

    // Open one playback device at the wire's speaker format (48 kHz stereo
    // S16), in queued mode and PAUSED: nothing plays until resumePlayback, so
    // the engine can build its start cushion first.
    virtual int openPlayback(const std::string& deviceName) = 0;

    // Queue interleaved stereo samples (sampleCount counts individual int16
    // values, not frames). False on an unknown handle or a failed queue.
    virtual bool queuePlayback(int handle, const std::int16_t* samples,
                               std::size_t sampleCount) = 0;

    // Bytes currently queued and unplayed; 0 for an unknown handle.
    virtual std::size_t queuedPlaybackBytes(int handle) = 0;

    // Start (unpause) a playback handle. Idempotent.
    virtual void resumePlayback(int handle) = 0;

    virtual void closePlayback(int handle) = 0;
};

} // namespace dish::source::audio
