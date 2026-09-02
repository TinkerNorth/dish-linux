// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller-audio wire (MSG_MIC_AUDIO 0x0012 up, MSG_SPEAKER_AUDIO 0x0013
// down, MSG_MIC_LED 0x0014 down): byte-exact framing over loopback UDP, the
// parse rules, the dispatch arms, and the two ceilings that keep an audio frame
// inside one datagram. The receive-buffer case at the bottom is the regression
// this feature forced: the pre-audio loop read into 256 bytes, and recvfrom
// TRUNCATES, so every full-size speaker frame would have failed the AEAD
// silently.

#include "Network/SatelliteClient.h"
#include "core/input/GamepadButtonLayouts.h"
#include "core/model/Protocol.h"
#include "core/wire/SessionCrypto.h"
#include "satellite_client_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

using dish::net::SatelliteClient;
using dish::net::SatelliteClientTestAccess;

namespace proto = dish::proto;

namespace {

std::array<std::uint8_t, 4> kToken{0x11, 0x22, 0x33, 0x44};
constexpr std::uint32_t kTokenBe = 0x11223344;

std::array<std::uint8_t, 32> key32(std::uint8_t fill) {
    std::array<std::uint8_t, 32> k{};
    k.fill(fill);
    return k;
}

int bindLoopback(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { return -1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    timeval rtv{};
    rtv.tv_sec = 0;
    rtv.tv_usec = 500'000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    return fd;
}

// A full datagram's worth of buffer, so the server side of a test never
// truncates what it is asserting about.
std::optional<std::vector<std::uint8_t>> recvDatagram(int fd, sockaddr_in* from = nullptr) {
    std::uint8_t buf[proto::kUdpDatagramMaxBytes];
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    const ssize_t n =
        ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
    if (n <= 0) { return std::nullopt; }
    if (from != nullptr) { *from = src; }
    return std::vector<std::uint8_t>(buf, buf + n);
}

struct LoopbackClient {
    int fd = -1;
    std::uint16_t port = 0;
    SatelliteClient client;

    LoopbackClient() {
        fd = bindLoopback(port);
        REQUIRE(fd >= 0);
        REQUIRE(client.openSocket("127.0.0.1", port));
        client.setConnectionParams(kToken, key32(0xA5), proto::kProtocolVersion);
    }
    ~LoopbackClient() {
        client.closeSocket();
        if (fd >= 0) { ::close(fd); }
    }
    LoopbackClient(const LoopbackClient&) = delete;
    LoopbackClient& operator=(const LoopbackClient&) = delete;
    LoopbackClient(LoopbackClient&&) = delete;
    LoopbackClient& operator=(LoopbackClient&&) = delete;
};

// Decrypt a client->server datagram back to the inner message. REQUIREs on
// structural failure so a test never asserts against garbage.
std::vector<std::uint8_t> decryptClientPacket(const std::vector<std::uint8_t>& pkt) {
    REQUIRE(pkt.size() >= 8 + 16);
    REQUIRE(std::memcmp(pkt.data(), kToken.data(), 4) == 0);
    const std::uint32_t ctr =
        (static_cast<std::uint32_t>(pkt[4]) << 24) | (static_cast<std::uint32_t>(pkt[5]) << 16) |
        (static_cast<std::uint32_t>(pkt[6]) << 8) | static_cast<std::uint32_t>(pkt[7]);
    std::vector<std::uint8_t> plain(pkt.size() - 8);
    unsigned long long plainLen = 0;
    REQUIRE(dish::wire::decryptPacket(key32(0xA5).data(), dish::wire::kDirClientToServer, ctr,
                                      kTokenBe, pkt.data() + 8, pkt.size() - 8, plain.data(),
                                      &plainLen));
    plain.resize(plainLen);
    return plain;
}

// Frame + encrypt a server->client message the way the satellite does:
// token(4) | counter(4 BE) | AEAD(inner), inner = msgType(BE) + len(BE) + body.
std::vector<std::uint8_t> serverPacket(std::uint16_t msgType, const std::vector<std::uint8_t>& body,
                                       std::uint32_t counter) {
    std::vector<std::uint8_t> inner(4 + body.size());
    inner[0] = static_cast<std::uint8_t>(msgType >> 8);
    inner[1] = static_cast<std::uint8_t>(msgType & 0xFF);
    inner[2] = static_cast<std::uint8_t>(body.size() >> 8);
    inner[3] = static_cast<std::uint8_t>(body.size() & 0xFF);
    if (!body.empty()) { std::memcpy(inner.data() + 4, body.data(), body.size()); }

    std::vector<std::uint8_t> pkt(8 + inner.size() + 16);
    std::memcpy(pkt.data(), kToken.data(), 4);
    pkt[4] = static_cast<std::uint8_t>(counter >> 24);
    pkt[5] = static_cast<std::uint8_t>(counter >> 16);
    pkt[6] = static_cast<std::uint8_t>(counter >> 8);
    pkt[7] = static_cast<std::uint8_t>(counter & 0xFF);
    unsigned long long ctLen = 0;
    REQUIRE(dish::wire::encryptPacket(key32(0xA5).data(), dish::wire::kDirServerToClient, counter,
                                      kTokenBe, inner.data(), inner.size(), pkt.data() + 8,
                                      &ctLen));
    pkt.resize(8 + ctLen);
    return pkt;
}

std::vector<std::uint8_t> audioBody(std::uint8_t ctrlIdx, std::uint16_t seq,
                                    const std::vector<std::uint8_t>& opus) {
    std::vector<std::uint8_t> body;
    body.push_back(ctrlIdx);
    body.push_back(static_cast<std::uint8_t>(seq >> 8));
    body.push_back(static_cast<std::uint8_t>(seq & 0xFF));
    body.insert(body.end(), opus.begin(), opus.end());
    return body;
}

} // namespace

// ── Constants ────────────────────────────────────────────────────────────────

TEST_CASE("the controller-audio opcodes and caps are the contract's values", "[audio][wire]") {
    CHECK(SatelliteClient::kMsgMicAudio == 0x0012);
    CHECK(SatelliteClient::kMsgSpeakerAudio == 0x0013);
    CHECK(SatelliteClient::kMsgMicLed == 0x0014);
    CHECK(SatelliteClient::kCapMic == 0x0040);
    CHECK(SatelliteClient::kCapSpeaker == 0x0080);
    // ...and neither collides with any cap that already existed.
    constexpr std::uint16_t kPrior =
        SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble |
        SatelliteClient::kCapMotion | SatelliteClient::kCapLightbar |
        SatelliteClient::kCapTriggerEffects | SatelliteClient::kCapPlayerLeds;
    CHECK((SatelliteClient::kCapMic & kPrior) == 0);
    CHECK((SatelliteClient::kCapSpeaker & kPrior) == 0);
    CHECK((SatelliteClient::kCapMic & SatelliteClient::kCapSpeaker) == 0);
    // The mic-mute state bit satellite-side consumes; pinned so the input
    // layout and this file cannot drift apart.
    CHECK(dish::input::layout::kXusbMicMute == 0x0800);
}

TEST_CASE("the audio caps helpers fold only their own bit", "[audio][wire]") {
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers;
    CHECK(SatelliteClient::withMicCapability(base, false) == base);
    CHECK(SatelliteClient::withMicCapability(base, true) == (base | SatelliteClient::kCapMic));
    CHECK(SatelliteClient::withSpeakerCapability(base, false) == base);
    CHECK(SatelliteClient::withSpeakerCapability(base, true) ==
          (base | SatelliteClient::kCapSpeaker));
    // Independent directions: folding both keeps both and neither implies the
    // other.
    const auto both = SatelliteClient::withSpeakerCapability(
        SatelliteClient::withMicCapability(base, true), true);
    CHECK((both & SatelliteClient::kCapMic) != 0);
    CHECK((both & SatelliteClient::kCapSpeaker) != 0);
    const auto micOnly = SatelliteClient::withMicCapability(base, true);
    CHECK((micOnly & SatelliteClient::kCapSpeaker) == 0);
}

// ── The 3-byte header ────────────────────────────────────────────────────────

TEST_CASE("the audio frame header is ctrlIdx then seq big-endian", "[audio][wire]") {
    const auto h = SatelliteClient::encodeAudioFrameHeader(7, 0xABCD);
    REQUIRE(h.size() == 3U);
    CHECK(h[0] == 7);
    CHECK(h[1] == 0xAB); // BE: high byte first, mirroring the satellite decoder
    CHECK(h[2] == 0xCD);
    // 0x0100 BE = 256; an LE encoder would emit 0x00 0x01 here.
    const auto boundary = SatelliteClient::encodeAudioFrameHeader(0, 0x0100);
    CHECK(boundary[1] == 0x01);
    CHECK(boundary[2] == 0x00);
}

// ── sendMicAudio: byte-exact over loopback ───────────────────────────────────

TEST_CASE("sendMicAudio frames ctrlIdx + seq BE + the verbatim Opus packet", "[audio][wire]") {
    LoopbackClient lb;
    const std::vector<std::uint8_t> opus{0xD0, 0xD1, 0xD2, 0xD3, 0xD4};
    REQUIRE(lb.client.sendMicAudio(3, 0x0102, opus.data(), opus.size()));

    const auto pkt = recvDatagram(lb.fd);
    REQUIRE(pkt.has_value());
    const auto inner = decryptClientPacket(*pkt);
    REQUIRE(inner.size() == 4U + 3U + opus.size());
    CHECK(inner[0] == 0x00);
    CHECK(inner[1] == 0x12); // MSG_MIC_AUDIO
    CHECK(inner[2] == 0x00);
    CHECK(inner[3] == 3 + opus.size()); // inner msgLen
    CHECK(inner[4] == 3);               // ctrlIdx
    CHECK(inner[5] == 0x01);            // seq BE
    CHECK(inner[6] == 0x02);
    CHECK(std::memcmp(inner.data() + 7, opus.data(), opus.size()) == 0);
}

TEST_CASE("a 1-byte DTX packet rides the wire; an empty one never does", "[audio][wire]") {
    LoopbackClient lb;
    const std::uint8_t dtx = 0xF8;

    // Empty is malformed by contract: the satellite would drop it, so the
    // client refuses to spend a datagram on it.
    CHECK_FALSE(lb.client.sendMicAudio(0, 1, nullptr, 1));
    CHECK_FALSE(lb.client.sendMicAudio(0, 1, &dtx, 0));
    CHECK_FALSE(recvDatagram(lb.fd).has_value());

    // A 1-byte packet is a legal DTX silence frame and the minimum the wire
    // carries: header + one Opus byte.
    REQUIRE(lb.client.sendMicAudio(0, 1, &dtx, 1));
    const auto pkt = recvDatagram(lb.fd);
    REQUIRE(pkt.has_value());
    const auto inner = decryptClientPacket(*pkt);
    CHECK(inner.size() == 4U + static_cast<std::size_t>(proto::kAudioWireMinPayloadBytes));
    CHECK(inner[7] == 0xF8);
}

TEST_CASE("seq wraps on the wire exactly as sent", "[audio][wire]") {
    LoopbackClient lb;
    const std::uint8_t opus = 0x01;
    REQUIRE(lb.client.sendMicAudio(0, 0xFFFF, &opus, 1));
    REQUIRE(lb.client.sendMicAudio(0, 0x0000, &opus, 1));
    const auto first = recvDatagram(lb.fd);
    const auto second = recvDatagram(lb.fd);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto a = decryptClientPacket(*first);
    const auto b = decryptClientPacket(*second);
    CHECK(a[5] == 0xFF);
    CHECK(a[6] == 0xFF);
    CHECK(b[5] == 0x00);
    CHECK(b[6] == 0x00);
}

// ── The datagram ceilings ────────────────────────────────────────────────────

TEST_CASE("the largest legal Opus packet fills the datagram exactly", "[audio][wire]") {
    LoopbackClient lb;
    const std::vector<std::uint8_t> opus(proto::kAudioWireMaxOpusBytes, 0x5A); // 1469
    REQUIRE(lb.client.sendMicAudio(1, 42, opus.data(), opus.size()));
    const auto pkt = recvDatagram(lb.fd);
    REQUIRE(pkt.has_value());
    // 8 outer + 4 inner + 3 header + 1469 opus + 16 tag = exactly one MTU.
    CHECK(pkt->size() == proto::kUdpDatagramMaxBytes);
    const auto inner = decryptClientPacket(*pkt);
    CHECK(inner.size() == 4U + proto::kUdpMaxInnerPayloadBytes);

    // One byte more is refused before it touches the socket.
    const std::vector<std::uint8_t> tooBig(proto::kAudioWireMaxOpusBytes + 1, 0x5A);
    CHECK_FALSE(lb.client.sendMicAudio(1, 43, tooBig.data(), tooBig.size()));
    CHECK_FALSE(recvDatagram(lb.fd).has_value());
}

TEST_CASE("the generic send framing takes 1472 payload bytes and refuses 1473", "[audio][wire]") {
    LoopbackClient lb;
    // At the ceiling: sent, and the datagram is exactly one MTU.
    const std::vector<std::uint8_t> atCap(proto::kUdpMaxInnerPayloadBytes, 0x33);
    CHECK(SatelliteClientTestAccess::sendEncrypted(lb.client, 0x0012, atCap.data(), atCap.size()));
    const auto sent = recvDatagram(lb.fd);
    REQUIRE(sent.has_value());
    CHECK(sent->size() == proto::kUdpDatagramMaxBytes);

    // One past it: refused, logged once, nothing on the wire — a fragmented
    // datagram would be truncated satellite-side and fail the AEAD anyway.
    const std::vector<std::uint8_t> overCap(proto::kUdpMaxInnerPayloadBytes + 1, 0x33);
    CHECK_FALSE(SatelliteClientTestAccess::sendEncrypted(lb.client, 0x0012, overCap.data(),
                                                         overCap.size()));
    CHECK_FALSE(recvDatagram(lb.fd).has_value());
    // The refusal must not wedge the sender.
    CHECK(SatelliteClientTestAccess::sendEncrypted(lb.client, 0x0012, atCap.data(), atCap.size()));
    CHECK(recvDatagram(lb.fd).has_value());
}

// ── parseSpeakerAudioMessage ─────────────────────────────────────────────────

TEST_CASE("SPEAKER_AUDIO parses header + Opus bytes with a floor of four", "[audio][wire]") {
    const std::vector<std::uint8_t> body = audioBody(2, 0xBEEF, {0x10, 0x20, 0x30});
    const auto msg = SatelliteClient::parseSpeakerAudioMessage(body.data(), body.size());
    REQUIRE(msg.has_value());
    CHECK(msg->controllerIndex == 2);
    CHECK(msg->seq == 0xBEEF);
    REQUIRE(msg->opusLen == 3U);
    // Borrowed, not copied: the pointer aliases the payload.
    CHECK(msg->opus == body.data() + 3);
    CHECK(msg->opus[0] == 0x10);
    CHECK(msg->opus[2] == 0x30);
}

TEST_CASE("a bare audio header with no Opus byte is malformed", "[audio][wire]") {
    // A silence frame is a 1-byte DTX packet, never an empty one, so a 3-byte
    // frame can only be a bug or an attack.
    const std::vector<std::uint8_t> body = audioBody(0, 7, {});
    CHECK(body.size() == 3U);
    CHECK_FALSE(SatelliteClient::parseSpeakerAudioMessage(body.data(), body.size()).has_value());
    CHECK_FALSE(SatelliteClient::parseSpeakerAudioMessage(body.data(), 2).has_value());
    CHECK_FALSE(SatelliteClient::parseSpeakerAudioMessage(body.data(), 0).has_value());
    CHECK_FALSE(SatelliteClient::parseSpeakerAudioMessage(nullptr, 8).has_value());

    // One Opus byte is the legal minimum (a DTX frame).
    const std::vector<std::uint8_t> dtx = audioBody(0, 7, {0xF8});
    const auto msg = SatelliteClient::parseSpeakerAudioMessage(dtx.data(), dtx.size());
    REQUIRE(msg.has_value());
    CHECK(msg->opusLen == 1U);
}

TEST_CASE("a full-size SPEAKER_AUDIO payload parses to 1469 Opus bytes", "[audio][wire]") {
    const std::vector<std::uint8_t> opus(proto::kAudioWireMaxOpusBytes, 0x77);
    const std::vector<std::uint8_t> body = audioBody(5, 1000, opus);
    CHECK(body.size() == proto::kUdpMaxInnerPayloadBytes);
    const auto msg = SatelliteClient::parseSpeakerAudioMessage(body.data(), body.size());
    REQUIRE(msg.has_value());
    CHECK(msg->opusLen == proto::kAudioWireMaxOpusBytes);
    CHECK(msg->seq == 1000);
}

// ── parseMicLedMessage ───────────────────────────────────────────────────────

TEST_CASE("MIC_LED parses the three lamp states and drops the rest", "[audio][wire]") {
    for (std::uint8_t state :
         {proto::kMicLedStateOff, proto::kMicLedStateOn, proto::kMicLedStatePulse}) {
        const std::uint8_t body[2] = {4, state};
        const auto msg = SatelliteClient::parseMicLedMessage(body, sizeof(body));
        REQUIRE(msg.has_value());
        CHECK(msg->controllerIndex == 4);
        CHECK(msg->state == state);
    }
    // An unknown state is dropped, not clamped: rendering a lamp mode the game
    // never asked for would be a guess at firmware.
    const std::uint8_t unknown[2] = {4, proto::kMicLedStateCount};
    CHECK_FALSE(SatelliteClient::parseMicLedMessage(unknown, sizeof(unknown)).has_value());
    const std::uint8_t wild[2] = {4, 0xFF};
    CHECK_FALSE(SatelliteClient::parseMicLedMessage(wild, sizeof(wild)).has_value());
}

TEST_CASE("MIC_LED takes exactly two bytes, no more and no fewer", "[audio][wire]") {
    const std::uint8_t body[3] = {1, 1, 0};
    CHECK(SatelliteClient::parseMicLedMessage(body, 2).has_value());
    CHECK_FALSE(SatelliteClient::parseMicLedMessage(body, 1).has_value());
    CHECK_FALSE(SatelliteClient::parseMicLedMessage(body, 3).has_value());
    CHECK_FALSE(SatelliteClient::parseMicLedMessage(body, 0).has_value());
    CHECK_FALSE(SatelliteClient::parseMicLedMessage(nullptr, 2).has_value());
}

// ── Dispatch through processIncoming ─────────────────────────────────────────

TEST_CASE("a SPEAKER_AUDIO datagram dispatches to the registered handler", "[audio][wire]") {
    SatelliteClient c;
    c.setConnectionParams(kToken, key32(0xA5), proto::kProtocolVersion);

    int calls = 0;
    std::vector<std::uint8_t> gotOpus;
    std::uint16_t gotSeq = 0;
    int gotIdx = -1;
    c.setSpeakerAudioHandler([&](const SatelliteClient::SpeakerAudioMessage& sm) {
        calls++;
        gotIdx = sm.controllerIndex;
        gotSeq = sm.seq;
        gotOpus.assign(sm.opus, sm.opus + sm.opusLen);
    });

    const std::vector<std::uint8_t> opus{0xAA, 0xBB, 0xCC};
    const auto pkt = serverPacket(0x0013, audioBody(1, 513, opus), /*counter=*/1);
    SatelliteClientTestAccess::processIncoming(c, pkt.data(), pkt.size());
    REQUIRE(calls == 1);
    CHECK(gotIdx == 1);
    CHECK(gotSeq == 513);
    CHECK(gotOpus == opus);

    // A malformed frame (header only) never reaches the handler.
    const auto bad = serverPacket(0x0013, audioBody(1, 514, {}), /*counter=*/2);
    SatelliteClientTestAccess::processIncoming(c, bad.data(), bad.size());
    CHECK(calls == 1);
}

TEST_CASE("a full-size 1500-byte datagram decrypts and dispatches whole", "[audio][wire]") {
    SatelliteClient c;
    c.setConnectionParams(kToken, key32(0xA5), proto::kProtocolVersion);

    std::size_t gotLen = 0;
    std::uint8_t firstByte = 0;
    std::uint8_t lastByte = 0;
    c.setSpeakerAudioHandler([&](const SatelliteClient::SpeakerAudioMessage& sm) {
        gotLen = sm.opusLen;
        firstByte = sm.opus[0];
        lastByte = sm.opus[sm.opusLen - 1];
    });

    std::vector<std::uint8_t> opus(proto::kAudioWireMaxOpusBytes, 0x00);
    opus.front() = 0x11;
    opus.back() = 0x99;
    const auto pkt = serverPacket(0x0013, audioBody(0, 1, opus), /*counter=*/1);
    REQUIRE(pkt.size() == proto::kUdpDatagramMaxBytes);
    SatelliteClientTestAccess::processIncoming(c, pkt.data(), pkt.size());
    CHECK(gotLen == proto::kAudioWireMaxOpusBytes);
    CHECK(firstByte == 0x11);
    CHECK(lastByte == 0x99);
}

TEST_CASE("a MIC_LED datagram dispatches state; junk states are dropped", "[audio][wire]") {
    SatelliteClient c;
    c.setConnectionParams(kToken, key32(0xA5), proto::kProtocolVersion);

    int calls = 0;
    std::uint8_t gotState = 0xFF;
    c.setMicLedHandler([&](const SatelliteClient::MicLedMessage& mm) {
        calls++;
        gotState = mm.state;
    });

    const auto pulse = serverPacket(0x0014, {2, proto::kMicLedStatePulse}, /*counter=*/1);
    SatelliteClientTestAccess::processIncoming(c, pulse.data(), pulse.size());
    REQUIRE(calls == 1);
    CHECK(gotState == proto::kMicLedStatePulse);

    const auto junk = serverPacket(0x0014, {2, 9}, /*counter=*/2);
    SatelliteClientTestAccess::processIncoming(c, junk.data(), junk.size());
    CHECK(calls == 1); // dropped, not clamped
}

// ── The receive loop's buffer ────────────────────────────────────────────────

TEST_CASE("receiveLoop survives a full-size datagram end to end", "[audio][wire]") {
    // The regression case: a 256-byte receive buffer truncates this datagram,
    // the AEAD fails on the truncated ciphertext, and the frame vanishes with
    // no error anywhere. Driving the REAL socket loop is the only thing that
    // pins the buffer size.
    LoopbackClient lb;

    std::atomic<int> got{0};
    std::atomic<std::size_t> gotLen{0};
    lb.client.setSpeakerAudioHandler([&](const SatelliteClient::SpeakerAudioMessage& sm) {
        gotLen.store(sm.opusLen);
        got.fetch_add(1);
    });
    lb.client.startReceiveLoop();

    // The client's socket is unbound until it sends; one heartbeat-sized send
    // teaches the server side its address.
    lb.client.sendBattery(0, 50, SatelliteClient::kBatteryStatusDischarging);
    sockaddr_in clientAddr{};
    REQUIRE(recvDatagram(lb.fd, &clientAddr).has_value());

    const std::vector<std::uint8_t> opus(proto::kAudioWireMaxOpusBytes, 0x42);
    const auto pkt = serverPacket(0x0013, audioBody(0, 77, opus), /*counter=*/1);
    REQUIRE(pkt.size() == proto::kUdpDatagramMaxBytes);
    ::sendto(lb.fd, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&clientAddr),
             sizeof(clientAddr));

    // Loopback delivery is fast; the deadline only bounds a genuine failure.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (got.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    lb.client.stopReceiveLoop();
    REQUIRE(got.load() == 1);
    CHECK(gotLen.load() == proto::kAudioWireMaxOpusBytes);
}
