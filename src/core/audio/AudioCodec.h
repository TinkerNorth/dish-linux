// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The codec seam for controller audio. Core owns the SHAPE of the two streams
// (formats and framing live in core/model/Protocol.h, mirroring satellite
// core/types.h); the library that actually codes them lives in
// source/audio/OpusAudioCodec.*, because src/core takes no third-party
// dependency — the same arrangement, for the same reason, as satellite's
// core/audio/audio_codec.h over adapters/audio/opus_codec.*.
//
// This client encodes mic and decodes speaker; the satellite does the reverse.
// The interfaces cover both halves anyway, because that is what lets a test
// close the loop on either stream instead of asserting against a second
// implementation of the same constants.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dish::audio {

// One controller's inbound stream. Stateful — decoders carry filter state and
// a concealment history across frames — so one instance per controller, never
// shared, and destroyed with the pad it belongs to.
//
// Every entry returns FRAMES (samples per channel) written, 0 on failure, and
// writes frames * channels interleaved int16 samples. `maxFrames` is capacity
// for decode(); for the two concealment entries it must be at least one whole
// proto::kAudioFrameSamples window, because the codec has to be told exactly
// how much audio is missing.
class IAudioDecoder {
  public:
    virtual ~IAudioDecoder() = default;
    virtual std::size_t decode(const std::uint8_t* opus, std::size_t opusLen, std::int16_t* pcm,
                               std::size_t maxFrames) = 0;

    // No packet at all: synthesize one frame from the decoder's own history.
    virtual std::size_t conceal(std::int16_t* pcm, std::size_t maxFrames) = 0;

    // Recover the frame BEFORE `opus` from the in-band FEC copy that packet
    // carries. Degrades to plain concealment when it turns out to carry none,
    // so a caller never has to ask first (nor could it: whether a packet holds
    // FEC data is an encoder-side decision made per packet).
    virtual std::size_t decodeFec(const std::uint8_t* opus, std::size_t opusLen, std::int16_t* pcm,
                                  std::size_t maxFrames) = 0;
};

// One controller's outbound stream. `frames` is per channel and must be exactly
// one proto::kAudioFrameSamples window: the wire carries one 20 ms packet per
// message and the windowing is the caller's job. Returns bytes written, 0 on
// failure.
class IAudioEncoder {
  public:
    virtual ~IAudioEncoder() = default;
    virtual std::size_t encode(const std::int16_t* pcm, std::size_t frames, std::uint8_t* out,
                               std::size_t maxOut) = 0;
};

} // namespace dish::audio
