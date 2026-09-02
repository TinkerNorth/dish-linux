// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "OpusAudioCodec.h"

#include "core/model/Protocol.h"

// Resolved as <opus.h>: every packaging of libopus we build against puts the
// headers in <prefix>/include/opus and puts THAT directory on the include path
// (the Opus::opus target's install interface under vcpkg, opus.pc's Cflags on
// msys2 / Debian / Homebrew), so the unqualified spelling is the portable one.
#include <opus.h>

#include <climits>

namespace dish::audio {
namespace {

// The contract's formats, in one place. Both streams are 48 kHz, 20 ms, VBR
// with in-band FEC requested; what differs is the application and the bitrate.
// Mirrored from satellite adapters/audio/opus_codec.cpp — the measurements
// quoted below are its, on the same libopus generation.
//
// Mic: 32 kbps mono under OPUS_APPLICATION_VOIP puts Opus in SILK mode, which
// is the only mode that HAS in-band FEC — the redundant low-rate copy of the
// previous frame that lets a receiver recover a single lost packet instead of
// guessing at it. The expected-loss hint is what makes the encoder actually
// spend bits on that copy; without it the flag alone does nothing.
//
// Speaker: 96 kbps stereo under OPUS_APPLICATION_AUDIO, because this carries
// game and chat audio a player listens to rather than speech a codec can model.
// The loss hint below, not the application, is what picks the mode: it forces
// SILK in, so BOTH streams encode as Hybrid fullband and both really do carry
// in-band FEC (measured on libopus 1.6.1: 8.4 dB recovery via decode_fec vs
// -1.3 dB for blind PLC on the speaker stream). Dropping the hint to zero would
// hand the speaker to CELT and silently delete that FEC.
constexpr int kOpusMicBitrateBps = 32000;
constexpr int kOpusSpeakerBitrateBps = 96000;
constexpr int kOpusExpectedPacketLossPct = 10;

int channelsFor(Stream stream) {
    return stream == Stream::Mic ? proto::kAudioMicChannels : proto::kAudioSpeakerChannels;
}

} // namespace

void OpusEncoderDeleter::operator()(::OpusEncoder* enc) const noexcept {
    if (enc != nullptr) { opus_encoder_destroy(enc); }
}

void OpusDecoderDeleter::operator()(::OpusDecoder* dec) const noexcept {
    if (dec != nullptr) { opus_decoder_destroy(dec); }
}

// ── decoder ─────────────────────────────────────────────────────────────────

std::unique_ptr<OpusStreamDecoder> OpusStreamDecoder::create(Stream stream) {
    const int channels = channelsFor(stream);
    int err = OPUS_OK;
    std::unique_ptr<::OpusDecoder, OpusDecoderDeleter> dec(
        opus_decoder_create(proto::kAudioSampleRateHz, channels, &err));
    if (!dec || err != OPUS_OK) { return nullptr; }
    // The decoder carries no format negotiation: a stream's parameters travel
    // inside each Opus packet, so there is nothing else to set here.
    return std::unique_ptr<OpusStreamDecoder>(new OpusStreamDecoder(std::move(dec), channels));
}

std::size_t OpusStreamDecoder::decode(const std::uint8_t* opus, std::size_t opusLen,
                                      std::int16_t* pcm, std::size_t maxFrames) {
    if (opus == nullptr || opusLen == 0 || pcm == nullptr || maxFrames == 0) { return 0; }
    if (opusLen > static_cast<std::size_t>(INT32_MAX) ||
        maxFrames > static_cast<std::size_t>(INT32_MAX)) {
        return 0;
    }
    const int n = opus_decode(dec_.get(), opus, static_cast<opus_int32>(opusLen), pcm,
                              static_cast<int>(maxFrames), /*decode_fec=*/0);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

std::size_t OpusStreamDecoder::conceal(std::int16_t* pcm, std::size_t maxFrames) {
    // A null packet is how libopus is asked for concealment, and unlike a real
    // decode the frame size is an instruction rather than a capacity: it must
    // be the duration of what is missing, which on this wire is always one
    // 20 ms window.
    if (pcm == nullptr || maxFrames < static_cast<std::size_t>(proto::kAudioFrameSamples)) {
        return 0;
    }
    const int n =
        opus_decode(dec_.get(), nullptr, 0, pcm, proto::kAudioFrameSamples, /*decode_fec=*/0);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

std::size_t OpusStreamDecoder::decodeFec(const std::uint8_t* opus, std::size_t opusLen,
                                         std::int16_t* pcm, std::size_t maxFrames) {
    if (opus == nullptr || opusLen == 0) { return conceal(pcm, maxFrames); }
    if (pcm == nullptr || maxFrames < static_cast<std::size_t>(proto::kAudioFrameSamples)) {
        return 0;
    }
    if (opusLen > static_cast<std::size_t>(INT32_MAX)) { return 0; }
    // decode_fec asks for the frame BEFORE this packet, so the frame size is
    // again the missing duration, not this packet's. libopus falls back to
    // concealment by itself when the packet carries no FEC data, which is why
    // callers can take this path unconditionally on a gap.
    const int n = opus_decode(dec_.get(), opus, static_cast<opus_int32>(opusLen), pcm,
                              proto::kAudioFrameSamples, /*decode_fec=*/1);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

// ── encoder ─────────────────────────────────────────────────────────────────

std::unique_ptr<OpusStreamEncoder> OpusStreamEncoder::create(Stream stream) {
    const int channels = channelsFor(stream);
    const int application = stream == Stream::Mic ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
    const int bitrate = stream == Stream::Mic ? kOpusMicBitrateBps : kOpusSpeakerBitrateBps;

    int err = OPUS_OK;
    std::unique_ptr<::OpusEncoder, OpusEncoderDeleter> enc(
        opus_encoder_create(proto::kAudioSampleRateHz, channels, application, &err));
    if (!enc || err != OPUS_OK) { return nullptr; }

    // Every ctl is checked: an encoder silently running at the wrong bitrate or
    // without FEC would degrade a live call in a way no test would catch later.
    if (opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(bitrate)) != OPUS_OK) { return nullptr; }
    if (opus_encoder_ctl(enc.get(), OPUS_SET_VBR(1)) != OPUS_OK) { return nullptr; }
    if (opus_encoder_ctl(enc.get(), OPUS_SET_INBAND_FEC(1)) != OPUS_OK) { return nullptr; }
    if (opus_encoder_ctl(enc.get(), OPUS_SET_PACKET_LOSS_PERC(kOpusExpectedPacketLossPct)) !=
        OPUS_OK) {
        return nullptr;
    }
    // Mic only. A live microphone never goes digitally silent, so DTX is the
    // only thing that can collapse a quiet room (satellite measured 123 of 250
    // frames gated at -50 dBFS after speech, 30.0 -> 16.4 kbps). The speaker
    // stream declines it on purpose: its gate cuts anything ~26-30 dB below the
    // recent peak, which on game audio means a reverb tail or quiet ambience is
    // replaced by comfort noise at -2.3 dB SNR. That direction relies on the
    // satellite's exact-silence suppression instead, which cannot touch audible
    // content.
    if (stream == Stream::Mic) {
        if (opus_encoder_ctl(enc.get(), OPUS_SET_DTX(1)) != OPUS_OK) { return nullptr; }
    }
    return std::unique_ptr<OpusStreamEncoder>(new OpusStreamEncoder(std::move(enc), channels));
}

std::size_t OpusStreamEncoder::encode(const std::int16_t* pcm, std::size_t frames,
                                      std::uint8_t* out, std::size_t maxOut) {
    // One wire message is exactly one 20 ms packet, so a caller handing a
    // different window has mis-framed rather than merely mis-sized: refuse it
    // instead of emitting a packet the other end cannot place in its timeline.
    if (pcm == nullptr || out == nullptr) { return 0; }
    if (frames != static_cast<std::size_t>(proto::kAudioFrameSamples)) { return 0; }
    if (maxOut == 0 || maxOut > static_cast<std::size_t>(INT32_MAX)) { return 0; }
    const int n = opus_encode(enc_.get(), pcm, proto::kAudioFrameSamples, out,
                              static_cast<opus_int32>(maxOut));
    // A 1-byte result is a legal silence packet, not a failure, and the wire
    // format is sized to carry one (proto::kAudioWireMinPayloadBytes).
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

} // namespace dish::audio
