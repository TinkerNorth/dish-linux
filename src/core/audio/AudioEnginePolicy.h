// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller-audio engine rules, in one place and with nothing else in
// them: when a microphone may be open, when a speaker voice may be held, when
// the descriptor may promise either, how arbitrary capture chunks become exact
// 20 ms windows, and when a drained playout queue is re-cushioned. The engines
// in source/audio execute these; keeping the decisions here is what makes them
// provable without a device.
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
// `toggleOn`, `routeMatched` and `hostCarries` are the facts the descriptor's
// audio cap is folded from as well (audioCapFactsOf), so a slot that captures
// or plays is always one the host was told about, and a cap the host was told
// is always one this end will honour; `streaming` is a bound slot on a live
// session.
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

// One descriptor audio cap's facts. A cap is a promise the host acts on alone:
// it streams, or lights the mute lamp, on the strength of the claim, and for
// the haptics lane it then sends no rumble reduction beside it. So the cap
// says what this end can and will carry right now, and nothing it cannot.
struct AudioCapFacts {
    bool routeMatched = false;
    bool toggleOn = false;
    // The host's probe grants the direction, as last read.
    bool hostGrants = false;
    // The audio stack refused the lane's endpoint as the route names it now
    // (core/audio/AudioRefusal.h).
    bool refused = false;
};

inline constexpr bool audioCapAdvertised(const AudioCapFacts& f) {
    return f.routeMatched && f.toggleOn && f.hostGrants && !f.refused;
}

// The cap's facts out of the slot's engine facts: the same route, switch and
// grant. Streaming is set aside because the descriptor is what brings the
// session up, and mute because the lamp the mic cap invites is the mute's own
// indicator.
inline constexpr AudioCapFacts audioCapFactsOf(const AudioSlotFacts& slot, bool refused) {
    AudioCapFacts cap;
    cap.routeMatched = slot.routeMatched;
    cap.toggleOn = slot.toggleOn;
    cap.hostGrants = slot.hostCarries;
    cap.refused = refused;
    return cap;
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

// The queue's ceiling, in frames: the cushion plus the largest burst one push
// of the reorder window can emit (AUDIO_JITTER_MAX_EVENTS_PER_PUSH), 160 ms.
// Queued audio is latency, and a queue that only ever grows never drains back
// to the cushion, so a window that would push past this is dropped rather than
// queued: a burst after a stall, or a device clock slower than the stream's,
// costs one window now and then instead of latency for the rest of the session.
inline constexpr int kPlayoutQueueCeilingFrames = 8;

inline constexpr bool playoutQueueFull(std::size_t queuedBytes, std::size_t frameBytes) {
    return queuedBytes >= static_cast<std::size_t>(kPlayoutQueueCeilingFrames) * frameBytes;
}

// A stream that restarted under a voice that outlived it (the satellite
// re-plugged the pad, or healed the session on the same connection) reads as
// late frames for as long as the old count runs ahead: up to ten minutes. This
// many late frames in a row, 200 ms and five reorder windows, is a restart and
// not reordering, and the voice primes a fresh window on the frame that proved
// it.
inline constexpr int kPlayoutLateRunRestart = 10;

inline constexpr bool playoutRestartNeeded(int lateRun) {
    return lateRun >= kPlayoutLateRunRestart;
}

// Frames waiting for the playout thread, oldest first; the oldest is dropped
// past this depth (160 ms), as dish-android drops, so a stalled decode can
// never hold the receive thread or grow the queue without bound.
inline constexpr int kPlayoutPendingFrames = 8;

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
