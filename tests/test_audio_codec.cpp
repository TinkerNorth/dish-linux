// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The libopus wrapper (source/audio/OpusAudioCodec.*): the two controller-audio
// stream formats, round-tripped through real encoders and decoders.
//
// The assertions are deliberately about SHAPE rather than samples. Opus is
// lossy and version-dependent, so pinning bytes would pin the library version;
// what must not drift is that a 20 ms window in comes back out as a 20 ms
// window, that a tone survives as a tone, that concealment produces audio for a
// frame that never arrived, and above all that the mic stream really carries
// in-band FEC. That last one is an encoder-setting question and exactly the
// sort of thing that silently stops being true: a stream with no FEC encodes,
// decodes and sounds perfect right up until the first packet goes missing.
//
// This client only encodes mic and decodes speaker, but both halves of both
// streams are built (OpusAudioCodec.h says why), so each stream's loop closes
// here instead of against a second implementation of the same constants. A
// mirror of dish-android's audio_codec_test.cpp, case for case.

#include "source/audio/OpusAudioCodec.h"

#include "core/audio/AudioJitter.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace proto = dish::proto;

namespace {

using dish::audio::OpusStreamDecoder;
using dish::audio::OpusStreamEncoder;
using dish::audio::Stream;

constexpr std::size_t kMicFrame = static_cast<std::size_t>(proto::kAudioFrameSamples);
constexpr std::size_t kSpeakerFrame =
    static_cast<std::size_t>(proto::kAudioFrameSamples * proto::kAudioSpeakerChannels);
// Comfortably past a 96 kbps 20 ms packet (~240 bytes) without being the wire
// ceiling, so a wildly oversized packet would still be visible as one.
constexpr std::size_t kMaxPacket = 1024;
constexpr double kPi = 3.14159265358979;

// Speech-ish content: a 220 Hz fundamental plus two harmonics, amplitude
// modulated so successive frames differ. Steady silence would let a
// concealment path look identical to a real decode and prove nothing.
void fillMicFrame(std::vector<std::int16_t>& pcm, int frameIndex) {
    pcm.resize(kMicFrame);
    for (std::size_t i = 0; i < kMicFrame; i++) {
        const double t = (frameIndex * static_cast<double>(kMicFrame) + static_cast<double>(i)) /
                         proto::kAudioSampleRateHz;
        const double env = 0.55 + 0.45 * std::sin(2.0 * kPi * 3.0 * t);
        const double s = std::sin(2.0 * kPi * 220.0 * t) + 0.5 * std::sin(2.0 * kPi * 440.0 * t) +
                         0.25 * std::sin(2.0 * kPi * 880.0 * t);
        pcm[i] = static_cast<std::int16_t>(env * s * 8000.0);
    }
}

// Stereo with the channels deliberately unequal, so a wrapper that collapsed or
// swapped them would show up as an energy imbalance rather than passing.
void fillSpeakerFrame(std::vector<std::int16_t>& pcm, int frameIndex) {
    pcm.resize(kSpeakerFrame);
    for (std::size_t i = 0; i < kMicFrame; i++) {
        const double t = (frameIndex * static_cast<double>(kMicFrame) + static_cast<double>(i)) /
                         proto::kAudioSampleRateHz;
        pcm[i * 2 + 0] = static_cast<std::int16_t>(std::sin(2.0 * kPi * 330.0 * t) * 9000.0);
        pcm[i * 2 + 1] = static_cast<std::int16_t>(std::sin(2.0 * kPi * 660.0 * t) * 4000.0);
    }
}

// Mean square over a channel of an interleaved buffer (stride 1 for mono).
double energy(const std::int16_t* pcm, std::size_t frames, int stride, int offset) {
    if (frames == 0) { return 0.0; }
    double sum = 0.0;
    for (std::size_t i = 0; i < frames; i++) {
        const double v =
            pcm[i * static_cast<std::size_t>(stride) + static_cast<std::size_t>(offset)];
        sum += v * v;
    }
    return sum / static_cast<double>(frames);
}

bool sameSamples(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}

// Encode a run of mic frames, drop `lost`, and decode the run twice from
// identical decoder state: once recovering the hole from the carrier packet's
// in-band FEC, once concealing it blind. Whether the two outputs differ is
// exactly "did packet lost+1 carry a redundant copy of frame lost".
bool fecBeatsPlcForFrame(int lost, double& outFecEnergy, double& outSourceEnergy) {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto decFec = OpusStreamDecoder::create(Stream::Mic);
    auto decPlc = OpusStreamDecoder::create(Stream::Mic);
    if (!enc || !decFec || !decPlc) { return false; }

    const int kFrames = 14;
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<std::vector<std::int16_t>> sources;
    for (int f = 0; f < kFrames; f++) {
        std::vector<std::int16_t> src;
        fillMicFrame(src, f);
        std::uint8_t buf[kMaxPacket];
        const std::size_t bytes = enc->encode(src.data(), kMicFrame, buf, sizeof(buf));
        if (bytes == 0) { return false; }
        packets.push_back(std::vector<std::uint8_t>(buf, buf + bytes));
        sources.push_back(src);
    }

    std::vector<std::int16_t> fecOut(kMicFrame, 0);
    std::vector<std::int16_t> plcOut(kMicFrame, 0);
    std::vector<std::int16_t> scratch(kMicFrame, 0);
    for (int f = 0; f < kFrames; f++) {
        const auto fi = static_cast<std::size_t>(f);
        if (f == lost) {
            // Recovered from packet f+1, which is what the jitter window hands
            // over as a gap's carrier. Order matters: the FEC copy is decoded
            // BEFORE the carrier's own frame.
            if (decFec->decodeFec(packets[fi + 1].data(), packets[fi + 1].size(), fecOut.data(),
                                  fecOut.size()) != kMicFrame) {
                return false;
            }
            if (decPlc->conceal(plcOut.data(), plcOut.size()) != kMicFrame) { return false; }
            continue;
        }
        decFec->decode(packets[fi].data(), packets[fi].size(), scratch.data(), scratch.size());
        decPlc->decode(packets[fi].data(), packets[fi].size(), scratch.data(), scratch.size());
    }

    outFecEnergy = energy(fecOut.data(), kMicFrame, 1, 0);
    outSourceEnergy = energy(sources[static_cast<std::size_t>(lost)].data(), kMicFrame, 1, 0);
    return !sameSamples(fecOut, plcOut);
}

} // namespace

TEST_CASE("wire audio constants match the contract", "[audio][codec]") {
    // A cross-repo agreement, not a tuning knob: satellite pins the same
    // numbers in core/types.h, and the wire never negotiates any of them.
    CHECK(proto::kAudioSampleRateHz == 48000);
    CHECK(proto::kAudioFrameMs == 20);
    CHECK(proto::kAudioFrameSamples == 960);
    CHECK(proto::kAudioMicChannels == 1);
    CHECK(proto::kAudioSpeakerChannels == 2);
    CHECK(proto::kAudioWireHeaderBytes == 3);
    CHECK(proto::kAudioWireMinPayloadBytes == 4);
    CHECK(proto::kUdpDatagramMaxBytes == 1500U);
    CHECK(proto::kUdpMaxInnerPayloadBytes == 1472U);
    CHECK(proto::kAudioWireMaxOpusBytes == 1469U);
}

TEST_CASE("mic round trip preserves the frame and the signal", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);
    CHECK(enc->channels() == proto::kAudioMicChannels);
    CHECK(dec->channels() == proto::kAudioMicChannels);

    std::vector<std::int16_t> src;
    std::vector<std::int16_t> out(kMicFrame * 2, 0);
    std::uint8_t packet[kMaxPacket];
    double lastEnergyRatio = 0.0;

    // Several frames: Opus needs a few to leave its start-up transient, and a
    // wrapper that only worked on frame 0 would be a real bug.
    for (int f = 0; f < 12; f++) {
        fillMicFrame(src, f);
        const std::size_t bytes = enc->encode(src.data(), kMicFrame, packet, sizeof(packet));
        CHECK(bytes > 0U);
        CHECK(bytes < sizeof(packet));
        CHECK(dec->decode(packet, bytes, out.data(), out.size()) == kMicFrame);
        if (f >= 4) {
            const double in = energy(src.data(), kMicFrame, 1, 0);
            const double got = energy(out.data(), kMicFrame, 1, 0);
            lastEnergyRatio = in > 0.0 ? got / in : 0.0;
            // Lossy, so not equal; but a codec that dropped the signal or blew
            // it up by an order of magnitude is broken, not lossy.
            CHECK(lastEnergyRatio > 0.3);
            CHECK(lastEnergyRatio < 3.0);
        }
    }
    CHECK(lastEnergyRatio > 0.0);

    // ~32 kbps at 20 ms is ~80 bytes; the assertion is only that VBR is not
    // running an order of magnitude off the configured rate.
    fillMicFrame(src, 20);
    const std::size_t bytes = enc->encode(src.data(), kMicFrame, packet, sizeof(packet));
    CHECK(bytes > 20U);
    CHECK(bytes < 400U);
}

TEST_CASE("every mic packet fits the wire ceiling with room to spare", "[audio][codec]") {
    // The send path hands the encoder the whole wire ceiling as its output
    // budget, so this is what keeps that generosity honest: a 20 ms mic packet
    // is two orders of magnitude below the datagram limit.
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    REQUIRE(enc != nullptr);
    std::vector<std::int16_t> src;
    std::vector<std::uint8_t> out(proto::kAudioWireMaxOpusBytes);
    for (int f = 0; f < 12; f++) {
        fillMicFrame(src, f);
        const std::size_t bytes = enc->encode(src.data(), kMicFrame, out.data(), out.size());
        CHECK(bytes > 0U);
        CHECK(static_cast<std::size_t>(proto::kAudioWireHeaderBytes) + bytes <=
              proto::kUdpMaxInnerPayloadBytes);
    }
}

TEST_CASE("speaker stereo round-trips with the channel imbalance intact", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    auto dec = OpusStreamDecoder::create(Stream::Speaker);
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);
    CHECK(enc->channels() == proto::kAudioSpeakerChannels);
    CHECK(dec->channels() == proto::kAudioSpeakerChannels);

    std::vector<std::int16_t> src;
    std::vector<std::int16_t> out(kSpeakerFrame * 2, 0);
    std::uint8_t packet[kMaxPacket];
    double leftOverRight = 0.0;
    for (int f = 0; f < 12; f++) {
        fillSpeakerFrame(src, f);
        const std::size_t bytes = enc->encode(src.data(), kMicFrame, packet, sizeof(packet));
        CHECK(bytes > 0U);
        CHECK(dec->decode(packet, bytes, out.data(), out.size() / 2) == kMicFrame);
        if (f >= 4) {
            const double l = energy(out.data(), kMicFrame, 2, 0);
            const double r = energy(out.data(), kMicFrame, 2, 1);
            leftOverRight = r > 0.0 ? l / r : 0.0;
        }
    }
    // Source left is ~5x right in power. A wrapper that downmixed to mono, or
    // swapped the interleave, would land near 1.0 or well under it.
    CHECK(leftOverRight > 1.5);
    CHECK(leftOverRight < 12.0);
}

TEST_CASE("encode refuses a window that is not exactly twenty milliseconds", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    REQUIRE(enc != nullptr);

    std::vector<std::int16_t> src;
    fillMicFrame(src, 0);
    std::uint8_t packet[kMaxPacket];
    // One wire message is one 20 ms packet: a caller handing a different window
    // has mis-framed, and emitting the packet anyway would put audio on the
    // wire the satellite cannot place in its timeline.
    CHECK(enc->encode(src.data(), kMicFrame - 1, packet, sizeof(packet)) == 0U);
    CHECK(enc->encode(src.data(), kMicFrame + 1, packet, sizeof(packet)) == 0U);
    CHECK(enc->encode(src.data(), 0, packet, sizeof(packet)) == 0U);
    CHECK(enc->encode(nullptr, kMicFrame, packet, sizeof(packet)) == 0U);
    CHECK(enc->encode(src.data(), kMicFrame, nullptr, sizeof(packet)) == 0U);
    CHECK(enc->encode(src.data(), kMicFrame, packet, 0) == 0U);
    // And the good call still works afterwards: a refusal must not wedge the
    // encoder.
    CHECK(enc->encode(src.data(), kMicFrame, packet, sizeof(packet)) > 0U);
}

TEST_CASE("garbage and truncated packets leave a usable decoder", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    auto dec = OpusStreamDecoder::create(Stream::Speaker);
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);

    std::vector<std::int16_t> out(kSpeakerFrame, 0);
    const std::uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // Not asserting failure: some byte strings ARE valid Opus. Asserting only
    // that nothing reads out of bounds and the decoder survives.
    (void)dec->decode(garbage, sizeof(garbage), out.data(), kMicFrame);
    CHECK(dec->decode(nullptr, 4, out.data(), kMicFrame) == 0U);
    CHECK(dec->decode(garbage, 0, out.data(), kMicFrame) == 0U);
    CHECK(dec->decode(garbage, sizeof(garbage), nullptr, kMicFrame) == 0U);
    CHECK(dec->decode(garbage, sizeof(garbage), out.data(), 0) == 0U);

    std::vector<std::int16_t> src;
    fillSpeakerFrame(src, 0);
    std::uint8_t packet[kMaxPacket];
    const std::size_t bytes = enc->encode(src.data(), kMicFrame, packet, sizeof(packet));
    REQUIRE(bytes > 4U);
    (void)dec->decode(packet, bytes / 2, out.data(), kMicFrame); // truncated
    // Whatever the malformed input did, a real packet still decodes.
    CHECK(dec->decode(packet, bytes, out.data(), kMicFrame) == kMicFrame);
}

TEST_CASE("a packet claiming more than twenty milliseconds is refused", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    auto dec = OpusStreamDecoder::create(Stream::Speaker);
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);

    std::vector<std::int16_t> src;
    fillSpeakerFrame(src, 0);
    std::uint8_t packet[kMaxPacket];
    const std::size_t bytes = enc->encode(src.data(), kMicFrame, packet, sizeof(packet));
    REQUIRE(bytes > 1U);

    // Forge a 40 ms packet out of the 20 ms one: Opus's TOC byte carries the
    // frame count in its low two bits, so code 1 (two equal frames) plus a
    // duplicated body is a structurally valid packet of twice the duration.
    // This is exactly what a hostile host could put on the wire, and the
    // dispatch decodes into a fixed kAudioFrameSamples buffer, so "refused"
    // and "not written past" have to be the same thing.
    std::vector<std::uint8_t> twoFrames;
    twoFrames.push_back(static_cast<std::uint8_t>((packet[0] & 0xFC) | 0x01));
    twoFrames.insert(twoFrames.end(), packet + 1, packet + bytes);
    twoFrames.insert(twoFrames.end(), packet + 1, packet + bytes);

    std::vector<std::int16_t> out(kSpeakerFrame, 0);
    CHECK(dec->decode(twoFrames.data(), twoFrames.size(), out.data(), kMicFrame) == 0U);
    // Given room for the whole 40 ms it decodes fine, which is what makes the
    // refusal above a capacity check rather than the packet being malformed.
    std::vector<std::int16_t> roomy(kSpeakerFrame * 2, 0);
    CHECK(dec->decode(twoFrames.data(), twoFrames.size(), roomy.data(), kMicFrame * 2) ==
          kMicFrame * 2);
}

TEST_CASE("concealment synthesizes a full frame with no packet at all", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    auto dec = OpusStreamDecoder::create(Stream::Speaker);
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);

    std::vector<std::int16_t> src;
    std::vector<std::int16_t> out(kSpeakerFrame, 0);
    std::uint8_t packet[kMaxPacket];
    for (int f = 0; f < 8; f++) {
        fillSpeakerFrame(src, f);
        const std::size_t bytes = enc->encode(src.data(), kMicFrame, packet, sizeof(packet));
        CHECK(dec->decode(packet, bytes, out.data(), kMicFrame) == kMicFrame);
    }

    std::vector<std::int16_t> concealed(kSpeakerFrame, 0);
    CHECK(dec->conceal(concealed.data(), kSpeakerFrame) == kMicFrame);
    // Extrapolated from the tone that came before, so it must not be silence.
    // (An all-zero frame is what a decoder that ignored the request would give.)
    CHECK(energy(concealed.data(), kMicFrame, 2, 0) > 1000.0);

    // Too small a buffer is a caller error, not a request to conceal less: the
    // codec has to be told exactly how much audio is missing.
    CHECK(dec->conceal(concealed.data(), kMicFrame - 1) == 0U);
    CHECK(dec->conceal(nullptr, kSpeakerFrame) == 0U);
}

TEST_CASE("in-band FEC recovers most lost mic frames where PLC alone cannot", "[audio][codec]") {
    // Every frame in a run, not one: whether a given packet carries LBRR is an
    // encoder decision made per packet, and a mode switch can make the two
    // decode paths agree for a frame on its own. A strict majority separates
    // the two worlds cleanly. This is the regression that pins
    // OPUS_SET_INBAND_FEC plus the expected-loss hint; nothing else would catch
    // losing them, because a stream without FEC sounds perfect until a packet
    // goes missing.
    const int first = 4;
    const int last = 11;
    const int trials = last - first + 1;
    int recovered = 0;
    double fecEnergy = 0.0;
    double sourceEnergy = 0.0;
    for (int lost = first; lost <= last; lost++) {
        double e = 0.0;
        double s = 0.0;
        if (fecBeatsPlcForFrame(lost, e, s)) {
            recovered++;
            fecEnergy = e;
            sourceEnergy = s;
        }
    }
    CHECK(recovered * 2 > trials);
    REQUIRE(recovered > 0);

    // And a recovery is audio, not a click: energy in the same league as what
    // was encoded for the frame that went missing.
    CHECK(sourceEnergy > 0.0);
    CHECK(fecEnergy > sourceEnergy * 0.1);
    CHECK(fecEnergy < sourceEnergy * 10.0);
}

TEST_CASE("decodeFec with no carrier conceals instead of failing", "[audio][codec]") {
    auto dec = OpusStreamDecoder::create(Stream::Speaker);
    REQUIRE(dec != nullptr);

    // The dispatch takes the FEC path unconditionally on a gap, because
    // whether a packet carries FEC is an encoder decision it cannot see, and
    // the reorder window hands over a null carrier whenever the next frame has
    // not arrived either. A null carrier therefore has to mean "conceal".
    std::vector<std::int16_t> out(kSpeakerFrame, 0);
    CHECK(dec->decodeFec(nullptr, 0, out.data(), kSpeakerFrame) == kMicFrame);
    CHECK(dec->decodeFec(nullptr, 0, out.data(), kMicFrame - 1) == 0U);
}

// DTX is asymmetric between the two streams on purpose, and the asymmetry is
// invisible from the header: only behaviour can pin it. The mic wants it
// because a live microphone never goes digitally silent, so a VAD gate is the
// only thing that can collapse a quiet room. The speaker must not have it,
// because its gate cuts anything ~26-30 dB below the recent peak, which on game
// audio turns a reverb tail into comfort noise.
TEST_CASE("sustained mic silence collapses to DTX packets", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    REQUIRE(enc != nullptr);

    const std::vector<std::int16_t> silence(kMicFrame, 0);
    std::uint8_t packet[kMaxPacket];

    // DTX needs a run of qualifying input before it engages (200 ms when
    // satellite measured it), so the steady state is what is asserted, not
    // frame 1. The 20-frame lead-in is deliberately looser than that figure so
    // this does not become a pin on one libopus version's ramp.
    std::size_t tiny = 0;
    std::size_t counted = 0;
    for (int i = 0; i < 100; i++) {
        const std::size_t bytes = enc->encode(silence.data(), kMicFrame, packet, sizeof(packet));
        REQUIRE(bytes > 0U);
        if (i >= 20) {
            counted++;
            if (bytes <= 2) { tiny++; }
        }
    }
    REQUIRE(counted > 0U);
    CHECK(tiny > counted * 3 / 4);
}

TEST_CASE("DTX packets stay legal on the wire and decode", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);

    const std::vector<std::int16_t> silence(kMicFrame, 0);
    std::uint8_t packet[kMaxPacket];
    std::size_t bytes = 0;
    for (int i = 0; i < 40; i++) {
        bytes = enc->encode(silence.data(), kMicFrame, packet, sizeof(packet));
    }
    REQUIRE(bytes >= 1U);
    REQUIRE(bytes <= 2U);

    // A 1-byte packet is a legal Opus frame, and the wire minimum exists so it
    // survives dispatch: header + at least one Opus byte.
    CHECK(bytes + static_cast<std::size_t>(proto::kAudioWireHeaderBytes) >=
          static_cast<std::size_t>(proto::kAudioWireMinPayloadBytes));

    std::vector<std::int16_t> out(kMicFrame, 12345);
    CHECK(dec->decode(packet, bytes, out.data(),
                      static_cast<std::size_t>(proto::kAudioFrameSamples)) == kMicFrame);

    // And the reorder window must take it, not reject it as a runt.
    dish::audio::AudioJitterWindow window;
    const auto pushed = window.push(0, packet, bytes);
    CHECK(pushed.accept == dish::audio::AudioJitterWindow::Accept::Ok);
}

TEST_CASE("DTX does not gate real speech", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    REQUIRE(enc != nullptr);

    std::vector<std::int16_t> pcm;
    std::uint8_t packet[kMaxPacket];
    std::size_t tiny = 0;
    for (int i = 0; i < 60; i++) {
        fillMicFrame(pcm, i);
        const std::size_t bytes = enc->encode(pcm.data(), kMicFrame, packet, sizeof(packet));
        REQUIRE(bytes > 0U);
        if (bytes <= 2) { tiny++; }
    }
    CHECK(tiny == 0U);
}

TEST_CASE("declining DTX keeps speaker silence a full packet", "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    REQUIRE(enc != nullptr);

    const std::vector<std::int16_t> silence(kSpeakerFrame, 0);
    std::uint8_t packet[kMaxPacket];
    std::size_t tiny = 0;
    for (int i = 0; i < 100; i++) {
        const std::size_t bytes = enc->encode(silence.data(), kMicFrame, packet, sizeof(packet));
        REQUIRE(bytes > 0U);
        if (bytes <= 2) { tiny++; }
    }
    // Not a bug being pinned: the satellite suppresses exact digital silence
    // before it ever reaches its speaker encoder, so the codec never needs a
    // VAD there — and this client's mirror of the encoder keeps the setting.
    CHECK(tiny == 0U);
}

TEST_CASE("a tight output buffer truncates the packet rather than overrunning it",
          "[audio][codec]") {
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    REQUIRE(enc != nullptr);

    std::vector<std::int16_t> src;
    fillSpeakerFrame(src, 3);
    // libopus treats max_data_bytes as a hard ceiling it encodes down to, so a
    // small buffer produces a smaller packet rather than a buffer overrun. The
    // wire ceiling is generous, but this guarantee is what makes passing
    // sizeof(buffer) safe at the call site.
    std::uint8_t tight[64];
    const std::size_t bytes = enc->encode(src.data(), kMicFrame, tight, sizeof(tight));
    CHECK(bytes > 0U);
    CHECK(bytes <= sizeof(tight));
}
