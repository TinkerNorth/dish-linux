// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The speaker playout pipeline: one open playback device per eligible slot
// (the pad's OWN speaker/headset endpoint), fed from the receive thread —
// reorder window (core/audio/AudioJitter.h) in, Opus decode with FEC/PLC on
// its gap events, whole 20 ms windows queued to the device.
//
// Playback starts only once the start cushion is queued (two frames, the same
// 40 ms the reorder window already costs), and a queue that drains mid-stream
// — the satellite suppresses digitally silent windows WITHOUT advancing seq,
// so multi-second holes are normal — is re-cushioned with silence rather than
// re-primed, so a short sound after a quiet stretch is delayed 40 ms instead
// of stranded (core/audio/AudioEnginePolicy.h owns both rules).
//
// A voice is addressed the way the frames arrive: (connectionId, controller
// index), resolved to a slot once per reconcile rather than once per frame —
// 50 frames a second per stream land on the receive thread, and that thread
// must not spend them walking binding tables.

#pragma once

#include "core/audio/AudioCodec.h"
#include "core/audio/AudioJitter.h"
#include "source/audio/AudioDeviceGateway.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dish::source::audio {

// One eligible slot's voice: where its frames come from and where they play.
struct SpeakerVoiceTarget {
    std::string connectionId;
    int controllerIndex = 0;
    std::string slotId;
    std::string playbackDeviceName;
};

class SpeakerPlayoutEngine {
  public:
    using DecoderFactory = std::function<std::unique_ptr<dish::audio::IAudioDecoder>()>;

    // The default factory builds the wire's speaker decoder (Opus stereo);
    // tests inject a fake to observe the FEC/PLC dispatch and the queue math.
    explicit SpeakerPlayoutEngine(AudioDeviceGateway* gateway, DecoderFactory decoderFactory = {});
    ~SpeakerPlayoutEngine();

    SpeakerPlayoutEngine(const SpeakerPlayoutEngine&) = delete;
    SpeakerPlayoutEngine& operator=(const SpeakerPlayoutEngine&) = delete;
    SpeakerPlayoutEngine(SpeakerPlayoutEngine&&) = delete;
    SpeakerPlayoutEngine& operator=(SpeakerPlayoutEngine&&) = delete;

    // Converge on exactly `targets` (main thread). A failed device open drops
    // the voice; the next reconcile is the retry.
    void reconcile(const std::vector<SpeakerVoiceTarget>& targets);

    // One MSG_SPEAKER_AUDIO frame, on the connection's receive thread. The
    // opus bytes are borrowed (the SatelliteClient message contract); they are
    // consumed before returning. A frame for a voice that is not held — the
    // caps said so, or the plan moved — is dropped, which is also what a host
    // that ignored the caps deserves.
    void deliver(const std::string& connectionId, int controllerIndex, std::uint16_t seq,
                 const std::uint8_t* opus, std::size_t opusLen);

    // Whether a playback device is open for the slot right now (UI/tests).
    bool playingFor(const std::string& slotId) const;

  private:
    struct Voice {
        int handle = kNoAudioDevice;
        std::string slotId;
        std::string deviceName;
        dish::audio::AudioJitterWindow window;
        std::unique_ptr<dish::audio::IAudioDecoder> decoder;
        bool started = false;
    };
    using VoiceKey = std::pair<std::string, int>;

    void closeLocked(Voice& voice);

    AudioDeviceGateway* gateway_;
    DecoderFactory decoderFactory_;
    // One mutex over the voice table: reconcile (main) against deliver
    // (receive threads). Held across a voice's decode+queue so a close cannot
    // free the decoder under a frame; both sides are bounded and 50 frames/s
    // per voice is nothing next to it.
    mutable std::mutex mtx_;
    std::map<VoiceKey, std::unique_ptr<Voice>> voices_;
};

} // namespace dish::source::audio
