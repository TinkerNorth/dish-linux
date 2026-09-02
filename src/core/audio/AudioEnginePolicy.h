// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller-audio engine rules, in one place and with nothing else in
// them: when a microphone may be open, when a speaker voice may be held, how
// arbitrary capture chunks become exact 20 ms windows, and when a drained
// playout queue is re-cushioned. The engines in source/audio execute these;
// keeping the decisions here is what makes them provable without a device.
//
// The sibling of dish-android's MicCapturePolicy / SpeakerPlayoutPolicy /
// SpeakerCushionPolicy, folded into one header because this client's inputs
// are already flat bools (no permission broker, no foreground-service arming).

#pragma once

#include "core/model/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dish::audio {

// Everything the eligibility rules know about one slot, flattened out of the
// capability model, the stores and the connection so the rules stay pure.
// `toggleOn` and `routeMatched` are the same pair the descriptor's audio cap
// was folded from, so a slot that captures or plays is always one the host was
// told about; `hostCarries` is the live probe verdict, which the caps
// deliberately do not include; `streaming` is a bound slot on a live session.
struct AudioSlotFacts {
    bool streaming = false;
    bool toggleOn = false;
    bool routeMatched = false;
    bool hostCarries = false;
    bool muted = false;
};

// THE PRIVACY INVARIANT's decision half: any one fact going false closes the
// capture device — closes, not silences — and mute is one of them. The engine
// enforces it by never being handed an ineligible target, and the tests pin
// that an ineligible slot means zero sendMicAudio calls, not quiet ones.
inline constexpr bool micCaptureEligible(const AudioSlotFacts& f) {
    return f.streaming && f.toggleOn && f.routeMatched && f.hostCarries && !f.muted;
}

// Mute deliberately does NOT gate playout: the directions are not symmetric.
// This stream is the user's own PC sending to the user's own pad, and muting
// the microphone should not also silence the game.
inline constexpr bool speakerPlayoutEligible(const AudioSlotFacts& f) {
    return f.streaming && f.toggleOn && f.routeMatched && f.hostCarries;
}

// Playback starts once this many whole frames are queued, not on the first: a
// device started empty plays one window and then underruns, and every underrun
// is an audible click. Two windows is the same 40 ms the reorder window
// upstream already costs, so this adds no latency the stream did not have.
inline constexpr int kPlayoutStartThresholdFrames = 2;

// The device-side buffer request, in frames: room for a scheduling hiccup
// without letting the queue grow into latency. The start threshold decides the
// steady-state latency, not this.
inline constexpr int kPlayoutBufferFrames = 4;

inline constexpr std::size_t kPlayoutFrameBytes =
    static_cast<std::size_t>(proto::kAudioFrameSamples) *
    static_cast<std::size_t>(proto::kAudioSpeakerChannels) * sizeof(std::int16_t);

// How many frames of SILENCE to slip in front of the next window when the
// queue has fully drained mid-stream. The satellite sends nothing for a
// digitally silent window, so a live stream goes quiet for seconds at a time
// and the queue empties; resuming into an empty queue is the underrun the
// start threshold exists to prevent. Silence rather than a re-prime, because
// withholding windows until the threshold refills would strand a sound shorter
// than the cushion; writing silence delays the resumed audio by the same 40 ms
// and can never swallow it.
inline constexpr int playoutRefillFrames(bool started, std::size_t queuedBytes) {
    return (started && queuedBytes == 0) ? kPlayoutStartThresholdFrames : 0;
}

// Reassembles a capture stream into exact wire windows. SDL delivers whatever
// chunk the platform's period produced, and the wire takes exactly one 20 ms
// window per message — a partial window is never sent, because the far end
// cannot place one in its timeline. Carries the remainder between chunks.
class FrameWindower {
  public:
    explicit FrameWindower(int frameSamples = proto::kAudioFrameSamples)
        : frame_(static_cast<std::size_t>(frameSamples)) {
        buffer_.reserve(frame_);
    }

    // Feed one chunk; `emitWindow` fires once per COMPLETE window, with a
    // pointer valid only for the duration of the call. (Not named `emit`: Qt
    // defines that as a macro and this header reaches Qt translation units.)
    void feed(const std::int16_t* samples, std::size_t count,
              const std::function<void(const std::int16_t*, std::size_t)>& emitWindow) {
        if (samples == nullptr || !emitWindow) { return; }
        std::size_t i = 0;
        // Fast path: with nothing carried over, whole windows go straight from
        // the caller's buffer with no copy.
        if (buffer_.empty()) {
            while (count - i >= frame_) {
                emitWindow(samples + i, frame_);
                i += frame_;
            }
        }
        while (i < count) {
            const std::size_t take =
                frame_ - buffer_.size() < count - i ? frame_ - buffer_.size() : count - i;
            buffer_.insert(buffer_.end(), samples + i, samples + i + take);
            i += take;
            if (buffer_.size() == frame_) {
                emitWindow(buffer_.data(), frame_);
                buffer_.clear();
            }
        }
    }

    // A fresh stream must not inherit the tail of the previous one.
    void reset() { buffer_.clear(); }

    std::size_t pending() const { return buffer_.size(); }

  private:
    std::size_t frame_;
    std::vector<std::int16_t> buffer_;
};

} // namespace dish::audio
