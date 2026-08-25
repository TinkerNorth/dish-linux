// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Byte-exact encoder fixtures come from the Moonlight protocol documentation's
// "network" rows (Wolf docs, input-data.adoc) and Wolf's captured-session test
// payloads; the decoders are exercised over every host->client event this
// client handles, including short and malformed buffers.

#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightWire.h"

#include "Util/Hex.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace dish;
using namespace dish::moonwire;

namespace {

std::string hexOf(const std::uint8_t* data, std::size_t len) { return util::toHex(data, len); }

std::vector<std::uint8_t> bytesOf(const std::string& hex) {
    const auto decoded = util::fromHex(hex);
    REQUIRE(decoded.has_value());
    return *decoded;
}

} // namespace

TEST_CASE("MOUSE_MOVE_REL matches the documented network fixture", "[moonlight][wire]") {
    // input-data.adoc, network row: 06 02 0C 00 00 00 00 08 07 00 00 00 FF FF 00 00
    // = delta (-1, 0), deltas big-endian.
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::size_t len = encodeMouseMoveRel(buf.data(), -1, 0);
    CHECK(len == kMouseMoveRelSize);
    CHECK(hexOf(buf.data(), len) == "06020c000000000807000000ffff0000");
}

TEST_CASE("CONTROLLER_MULTI matches the captured-session fixture", "[moonlight][wire]") {
    // Wolf testControl.cpp "control joypad input packets": controller 0, active
    // mask 1, A pressed, everything else neutral.
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::size_t len =
        encodeControllerMulti(buf.data(), 0, 0x0001, moonproto::kBtnA, 0, 0, 0, 0, 0, 0);
    CHECK(len == kControllerMultiSize);
    CHECK(hexOf(buf.data(), len) ==
          "060222000000001e0c0000001a000000010014000010000000000000000000009c0000005500");
}

TEST_CASE("CONTROLLER_MULTI splits the extended button word", "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::uint32_t buttons = moonproto::kBtnA | moonproto::kBtnTouchpad; // hi and lo halves
    const std::size_t len =
        encodeControllerMulti(buf.data(), 1, 0x0003, buttons, 0xFF, 0x80, 100, -100, 32767, -32768);
    REQUIRE(len == kControllerMultiSize);
    // btnflags (offset 20) = 0x1000 LE; buttonFlags2 (offset 34) = 0x0010 LE.
    CHECK(buf[20] == 0x00);
    CHECK(buf[21] == 0x10);
    CHECK(buf[34] == 0x10);
    CHECK(buf[35] == 0x00);
    // ctrl# 1, active mask 3.
    CHECK(buf[14] == 0x01);
    CHECK(buf[16] == 0x03);
    // Triggers and stick extremes land at their fixed offsets.
    CHECK(buf[22] == 0xFF);
    CHECK(buf[23] == 0x80);
    CHECK(buf[28] == 0xFF); // 32767 = FF 7F
    CHECK(buf[29] == 0x7F);
    CHECK(buf[30] == 0x00); // -32768 = 00 80
    CHECK(buf[31] == 0x80);
}

TEST_CASE("CONTROLLER_ARRIVAL layout", "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::uint8_t caps = moonproto::kCapAnalogTriggers | moonproto::kCapRumble;
    const std::size_t len = encodeControllerArrival(buf.data(), 2, moonproto::kControllerTypePs,
                                                    caps, moonproto::kStandardButtons);
    CHECK(len == kControllerArrivalSize);
    // [06 02][0F 00][00 00 00 0B][04 00 00 55][ctrl][type][cap][buttons u32 LE]
    CHECK(hexOf(buf.data(), len) == "06020f000000000b04000055020203fff70000");
}

TEST_CASE("CONTROLLER_BATTERY layout", "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::size_t len = encodeControllerBattery(buf.data(), 0, moonproto::kBatteryCharging, 42);
    CHECK(len == kControllerBatterySize);
    CHECK(hexOf(buf.data(), len) == "06020c00000000080700005500032a00");
}

TEST_CASE("CONTROLLER_MOTION layout carries little-endian floats", "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::size_t len =
        encodeControllerMotion(buf.data(), 3, moonproto::kMotionGyroscope, 1.0F, -2.5F, 0.0F);
    CHECK(len == kControllerMotionSize);
    // Header: [06 02][18 00][00 00 00 14][06 00 00 55], body ctrl=3 type=2.
    CHECK(hexOf(buf.data(), 12) == "060218000000001406000055");
    CHECK(buf[12] == 3);
    CHECK(buf[13] == moonproto::kMotionGyroscope);
    // 1.0f = 0x3F800000 little-endian.
    CHECK(hexOf(buf.data() + 16, 4) == "0000803f");
    // -2.5f = 0xC0200000.
    CHECK(hexOf(buf.data() + 20, 4) == "000020c0");
    CHECK(hexOf(buf.data() + 24, 4) == "00000000");
}

TEST_CASE("PERIODIC_PING is byte-for-byte the captured plaintext", "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::size_t len = encodePeriodicPing(buf.data());
    CHECK(len == kPeriodicPingSize);
    CHECK(hexOf(buf.data(), len) == "000208000400000000000000");
}

TEST_CASE("TERMINATION carries the graceful reason big-endian", "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::size_t len = encodeTermination(buf.data());
    CHECK(len == kTerminationSize);
    CHECK(hexOf(buf.data(), len) == "090104008003" + std::string("0023"));
}

TEST_CASE("RTP ping falls back to the legacy 4-byte PING without a payload", "[moonlight][wire]") {
    std::array<std::uint8_t, kRtpPingSize> buf{};
    CHECK(encodeRtpPing(buf.data(), nullptr, 0, 7) == kRtpPingLegacySize);
    CHECK(hexOf(buf.data(), kRtpPingLegacySize) == "50494e47"); // "PING"

    const char empty[] = "";
    CHECK(encodeRtpPing(buf.data(), empty, 0, 7) == kRtpPingLegacySize);
    CHECK(hexOf(buf.data(), kRtpPingLegacySize) == "50494e47");
}

TEST_CASE("RTP ping echoes the SETUP payload as SS_PING", "[moonlight][wire]") {
    // The 16-char X-SS-Ping-Payload verbatim, then the sequence little-endian.
    std::array<std::uint8_t, kRtpPingSize> buf{};
    const std::string payload = "AbCd0123EfGh4567";
    const std::size_t len = encodeRtpPing(buf.data(), payload.data(), payload.size(), 0x01020304);
    CHECK(len == kRtpPingSize);
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), 16) == payload);
    CHECK(hexOf(buf.data() + 16, 4) == "04030201");
}

TEST_CASE("RTP ping pads a short payload and truncates a long one", "[moonlight][wire]") {
    std::array<std::uint8_t, kRtpPingSize> buf{};

    const std::string shortPayload = "abc";
    REQUIRE(encodeRtpPing(buf.data(), shortPayload.data(), shortPayload.size(), 1) == kRtpPingSize);
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), 3) == "abc");
    // Zero-padded through the fixed 16-byte field.
    CHECK(hexOf(buf.data() + 3, 13) == "00000000000000000000000000");
    CHECK(hexOf(buf.data() + 16, 4) == "01000000");

    const std::string longPayload = "0123456789abcdefEXTRA";
    REQUIRE(encodeRtpPing(buf.data(), longPayload.data(), longPayload.size(), 2) == kRtpPingSize);
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), 16) == "0123456789abcdef");
    CHECK(hexOf(buf.data() + 16, 4) == "02000000");
}

// ── Host -> client decoding ──────────────────────────────────────────────────

TEST_CASE("decodes RUMBLE_DATA", "[moonlight][wire]") {
    // [0b 01][0a 00] [unused u32][ctrl=1][low=0x1234][high=0xff00]
    const auto pkt = bytesOf("0b010a000000000001003412" + std::string("00ff"));
    const auto ev = decodeHostEvent(pkt.data(), pkt.size());
    REQUIRE(ev.has_value());
    CHECK(ev->type == HostEventType::Rumble);
    CHECK(ev->controllerNumber == 1);
    CHECK(ev->rumbleLow == 0x1234);
    CHECK(ev->rumbleHigh == 0xFF00);
}

TEST_CASE("decodes RUMBLE_TRIGGERS", "[moonlight][wire]") {
    const auto pkt = bytesOf("0055060002000a00" + std::string("1400"));
    const auto ev = decodeHostEvent(pkt.data(), pkt.size());
    REQUIRE(ev.has_value());
    CHECK(ev->type == HostEventType::RumbleTriggers);
    CHECK(ev->controllerNumber == 2);
    CHECK(ev->rumbleLow == 10);
    CHECK(ev->rumbleHigh == 20);
}

TEST_CASE("decodes MOTION_EVENT", "[moonlight][wire]") {
    // ctrl=0, rate=100 Hz, type=gyro.
    const auto pkt = bytesOf("015505000000640002");
    const auto ev = decodeHostEvent(pkt.data(), pkt.size());
    REQUIRE(ev.has_value());
    CHECK(ev->type == HostEventType::MotionRequest);
    CHECK(ev->controllerNumber == 0);
    CHECK(ev->motionRateHz == 100);
    CHECK(ev->motionType == moonproto::kMotionGyroscope);
}

TEST_CASE("decodes RGB_LED", "[moonlight][wire]") {
    const auto pkt = bytesOf("0255050001001020" + std::string("30"));
    const auto ev = decodeHostEvent(pkt.data(), pkt.size());
    REQUIRE(ev.has_value());
    CHECK(ev->type == HostEventType::RgbLed);
    CHECK(ev->controllerNumber == 1);
    CHECK(ev->red == 0x10);
    CHECK(ev->green == 0x20);
    CHECK(ev->blue == 0x30);
}

TEST_CASE("decodes TERMINATION from the host", "[moonlight][wire]") {
    const auto pkt = bytesOf("0901040080030023");
    const auto ev = decodeHostEvent(pkt.data(), pkt.size());
    REQUIRE(ev.has_value());
    CHECK(ev->type == HostEventType::Termination);
}

TEST_CASE("unknown types decode as Unknown, not an error", "[moonlight][wire]") {
    const auto pkt = bytesOf("0e01020000ff"); // HDR_MODE, unhandled
    const auto ev = decodeHostEvent(pkt.data(), pkt.size());
    REQUIRE(ev.has_value());
    CHECK(ev->type == HostEventType::Unknown);
}

TEST_CASE("short and malformed buffers are rejected", "[moonlight][wire]") {
    CHECK_FALSE(decodeHostEvent(nullptr, 100).has_value());

    const auto tooShortHeader = bytesOf("0b01");
    CHECK_FALSE(decodeHostEvent(tooShortHeader.data(), tooShortHeader.size()).has_value());

    // RUMBLE_DATA with a truncated body (9 of 10 bytes).
    const auto shortRumble = bytesOf("0b010a00000000000100341200");
    CHECK_FALSE(decodeHostEvent(shortRumble.data(), shortRumble.size() - 4).has_value());

    // RUMBLE_TRIGGERS truncated.
    const auto shortTriggers = bytesOf("00550600020070");
    CHECK_FALSE(decodeHostEvent(shortTriggers.data(), shortTriggers.size()).has_value());

    // MOTION_EVENT truncated (missing the type byte).
    const auto shortMotion = bytesOf("01550500000064");
    CHECK_FALSE(decodeHostEvent(shortMotion.data(), shortMotion.size() - 1).has_value());

    // RGB_LED truncated.
    const auto shortLed = bytesOf("02550500010010");
    CHECK_FALSE(decodeHostEvent(shortLed.data(), shortLed.size() - 1).has_value());

    // Empty buffer.
    const std::uint8_t byte = 0;
    CHECK_FALSE(decodeHostEvent(&byte, 0).has_value());
}
