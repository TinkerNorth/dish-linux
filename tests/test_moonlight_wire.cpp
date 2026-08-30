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
    // [06 02][10 00][00 00 00 0C][04 00 00 55][ctrl][type][cap][pad][buttons u32 LE]
    // The advertised word is the whole 16-bit legacy half: a live Sunshine host
    // logs it back as supportedButtonFlags [0000FFFF], and all three Dish
    // clients advertise the same value so one host cannot see three pads.
    CHECK(hexOf(buf.data(), len) == "060210000000000c0400005502020300ffff0000");
    CHECK(moonproto::kStandardButtons == 0x0000FFFFU);
}

TEST_CASE("CONTROLLER_ARRIVAL carries the struct's alignment pad", "[moonlight][wire]") {
    // THE BODY IS EIGHT BYTES, NOT SEVEN. The fields add up to seven, but the
    // host reads them out of a naturally aligned struct, so the u32 button mask
    // starts at offset 4 and offset 3 is reserved. Sending seven shifted every
    // field after the type by one and a live Sunshine host logged our
    // capabilities 0x03 as `capabilities [FF03]` and our 0xFFFF button mask as
    // `supportedButtonFlags [000000FF]`.
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    const std::uint8_t caps = moonproto::kCapAnalogTriggers | moonproto::kCapRumble;
    const std::size_t len =
        encodeControllerArrival(buf.data(), 0, moonproto::kControllerTypeXbox, caps, 0xFFFF);
    REQUIRE(len == 20);
    CHECK(hexOf(buf.data(), len) == "060210000000000c0400005500010300ffff0000");

    // Read back the way the host does: fixed offsets into the aligned struct.
    const std::uint8_t* body = buf.data() + 12;
    CHECK(body[0] == 0x00);                           // controller number
    CHECK(body[1] == moonproto::kControllerTypeXbox); // type
    CHECK(body[2] == caps);                           // capabilities
    CHECK(body[3] == 0x00);                           // reserved / alignment pad
    const std::uint32_t buttons =
        static_cast<std::uint32_t>(body[4]) | (static_cast<std::uint32_t>(body[5]) << 8) |
        (static_cast<std::uint32_t>(body[6]) << 16) | (static_cast<std::uint32_t>(body[7]) << 24);
    CHECK(buttons == 0x0000FFFFU);
    // The two words the host's own log prints, in its own spelling.
    CHECK(static_cast<std::uint16_t>(body[2]) == 0x0003U);

    // The wrapper counts the eight-byte body: packet_len 16, data_size 12.
    CHECK(hexOf(buf.data() + 2, 2) == "1000");
    CHECK(hexOf(buf.data() + 4, 4) == "0000000c");
}

TEST_CASE("CONTROLLER_ARRIVAL spans the whole emulated-type and capability range",
          "[moonlight][wire]") {
    std::array<std::uint8_t, kMaxPlaintextSize> buf{};
    for (const std::uint8_t type :
         {moonproto::kControllerTypeUnknown, moonproto::kControllerTypeXbox,
          moonproto::kControllerTypePs, moonproto::kControllerTypeNintendo}) {
        const std::uint8_t caps = static_cast<std::uint8_t>(
            moonproto::kCapAnalogTriggers | moonproto::kCapRumble | moonproto::kCapTriggerRumble |
            moonproto::kCapTouchpad | moonproto::kCapAccelerometer | moonproto::kCapGyro |
            moonproto::kCapBattery | moonproto::kCapRgbLed);
        REQUIRE(encodeControllerArrival(buf.data(), 3, type, caps, 0xFFFFFFFFU) ==
                kControllerArrivalSize);
        CHECK(buf[13] == type);
        CHECK(buf[14] == 0xFF);
        CHECK(buf[15] == 0x00);
        CHECK(hexOf(buf.data() + 16, 4) == "ffffffff");
    }
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

TEST_CASE("SS_PING is the live host's payload verbatim, not hex-decoded", "[moonlight][wire]") {
    // A live Sunshine host sent X-SS-Ping-Payload: 68A75BBEEEA86826. It LOOKS
    // like hex and is not: the host mints 16 printable ASCII characters and
    // matches the session by those same 16 bytes. Hex-decoding it produces an
    // 8-byte datagram, which lands in the 5..19 dead zone Wolf's udp-ping.cpp
    // discards without a word.
    std::array<std::uint8_t, kRtpPingSize> buf{};
    const std::string payload = "68A75BBEEEA86826";
    REQUIRE(payload.size() == 16);
    const std::size_t len = encodeRtpPing(buf.data(), payload.data(), payload.size(), 0);
    REQUIRE(len == 20);
    CHECK(hexOf(buf.data(), len) == "3638413735424245454541383638323600000000");
    // Byte for byte the header text; the hex decoding of it is 8 bytes long.
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), 16) == payload);
    const auto decoded = util::fromHex(payload);
    REQUIRE(decoded.has_value());
    CHECK(decoded->size() == 8);
    CHECK(len != decoded->size());
}

TEST_CASE("the RTP ping encoder never emits a 5..19 byte datagram", "[moonlight][wire]") {
    // LENGTH IS THE PROTOCOL. Wolf's rtp/udp-ping.cpp dispatches on the byte
    // count alone: exactly 4 is the legacy PING, 20 or more is an SS_PING, and
    // everything between is dropped silently, after which the host reports
    // "Initial Ping Timeout" and ends the session ten seconds in.
    std::array<std::uint8_t, kRtpPingSize> buf{};
    const std::string filler(40, 'x');
    for (std::size_t payloadLen = 0; payloadLen <= filler.size(); ++payloadLen) {
        const std::size_t len = encodeRtpPing(buf.data(), filler.data(), payloadLen, 1);
        CHECK((len == kRtpPingLegacySize || len == kRtpPingSize));
        CHECK_FALSE((len > kRtpPingLegacySize && len < kRtpPingSize));
    }
    CHECK(encodeRtpPing(buf.data(), nullptr, 0, 1) == kRtpPingLegacySize);
    CHECK(encodeRtpPing(buf.data(), nullptr, 16, 1) == kRtpPingLegacySize);
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
