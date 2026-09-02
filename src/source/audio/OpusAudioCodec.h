// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// libopus behind the core codec seam (core/audio/AudioCodec.h). Every knob the
// two controller-audio streams need is pinned in OpusAudioCodec.cpp; nothing
// about the format is negotiated at runtime, so this header is only about
// lifetime. Lives in source/ rather than core/ because it links a third-party
// library, the same layering as the sodium- and SDL-facing edges.
//
// Both directions of both streams are defined here even though this client only
// encodes mic and decodes speaker. The other two halves are what the satellite
// does, and having all four in one file is what lets a test close the loop on
// either stream instead of asserting against a second implementation of the
// same constants (satellite's opus_codec.h says the same thing from the other
// side).

#pragma once

#include "core/audio/AudioCodec.h"

#include <cstddef>
#include <cstdint>
#include <memory>

// libopus's handle types, forward-declared rather than #include <opus.h>: this
// header is included by tests and (in Wave 2) the audio engines, which have no
// reason to carry the codec's include path, and opus.h's own `typedef struct
// OpusEncoder OpusEncoder` agrees with these declarations.
struct OpusEncoder;
struct OpusDecoder;

namespace dish::audio {

// Which of the two wire streams an instance is pinned to. The distinction is
// not just channel count: the mic runs Opus's VOIP application at a bitrate
// where in-band FEC exists, the speaker runs the AUDIO application at a bitrate
// where fidelity matters more (OpusAudioCodec.cpp carries the numbers and why).
enum class Stream { Mic, Speaker };

// Declared here, defined in OpusAudioCodec.cpp, so the unique_ptrs below work
// against the incomplete handle types above.
struct OpusEncoderDeleter {
    void operator()(::OpusEncoder* enc) const noexcept;
};
struct OpusDecoderDeleter {
    void operator()(::OpusDecoder* dec) const noexcept;
};

class OpusStreamDecoder : public IAudioDecoder {
  public:
    // Null when libopus refuses to allocate. Callers treat that as "no codec"
    // rather than fatal: a controller without audio is still a controller.
    static std::unique_ptr<OpusStreamDecoder> create(Stream stream);

    std::size_t decode(const std::uint8_t* opus, std::size_t opusLen, std::int16_t* pcm,
                       std::size_t maxFrames) override;
    std::size_t conceal(std::int16_t* pcm, std::size_t maxFrames) override;
    std::size_t decodeFec(const std::uint8_t* opus, std::size_t opusLen, std::int16_t* pcm,
                          std::size_t maxFrames) override;

    int channels() const { return channels_; }

  private:
    OpusStreamDecoder(std::unique_ptr<::OpusDecoder, OpusDecoderDeleter> dec, int channels)
        : dec_(std::move(dec)), channels_(channels) {}

    std::unique_ptr<::OpusDecoder, OpusDecoderDeleter> dec_;
    int channels_ = 1;
};

class OpusStreamEncoder : public IAudioEncoder {
  public:
    // Null when libopus refuses to allocate; see OpusStreamDecoder::create.
    static std::unique_ptr<OpusStreamEncoder> create(Stream stream);

    std::size_t encode(const std::int16_t* pcm, std::size_t frames, std::uint8_t* out,
                       std::size_t maxOut) override;

    int channels() const { return channels_; }

  private:
    OpusStreamEncoder(std::unique_ptr<::OpusEncoder, OpusEncoderDeleter> enc, int channels)
        : enc_(std::move(enc)), channels_(channels) {}

    std::unique_ptr<::OpusEncoder, OpusEncoderDeleter> enc_;
    int channels_ = 2;
};

} // namespace dish::audio
