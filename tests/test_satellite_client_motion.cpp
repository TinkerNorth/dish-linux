// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// The MSG_MOTION / MSG_BATTERY payload layout must match the receiver's
// MotionReport / BatteryReport byte-for-byte. The receiver decodes with
// explicit little-endian byte shifts, not a struct memcpy, so the wire is
// independent of host byte order and struct layout.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;

namespace {

std::int16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

TEST_CASE("encodeMotionPayload places ctrlIdx at byte 0", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(/*ctrlIdx=*/7, 0, 0, 0, 0, 0, 0, 0);
    REQUIRE(out.size() == 17U);
    REQUIRE(out[0] == 7U);
}

TEST_CASE("encodeMotionPayload writes gyro / accel as LE int16", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(
        /*ctrlIdx=*/0, /*gx=*/0x0102, /*gy=*/0x0304, /*gz=*/0x0506,
        /*ax=*/0x0708, /*ay=*/0x090A, /*az=*/0x0B0C, /*dtUs=*/0);

    REQUIRE(readLe16(&out[1]) == 0x0102);
    REQUIRE(readLe16(&out[3]) == 0x0304);
    REQUIRE(readLe16(&out[5]) == 0x0506);
    REQUIRE(readLe16(&out[7]) == 0x0708);
    REQUIRE(readLe16(&out[9]) == 0x090A);
    REQUIRE(readLe16(&out[11]) == 0x0B0C);
}

TEST_CASE("encodeMotionPayload writes timestampDeltaUs as LE uint32", "[motion]") {
    const auto out =
        SatelliteClient::encodeMotionPayload(0, 0, 0, 0, 0, 0, 0, /*dtUs=*/0xDEADBEEFU);
    REQUIRE(readLe32(&out[13]) == 0xDEADBEEFU);
}

TEST_CASE("encodeMotionPayload handles full int16 range without overflow", "[motion]") {
    const auto out =
        SatelliteClient::encodeMotionPayload(0xFF, /*gx=*/-32768, /*gy=*/32767, /*gz=*/0,
                                             /*ax=*/-32768, /*ay=*/32767, /*az=*/-1, /*dtUs=*/0);
    REQUIRE(out[0] == 0xFFU);
    REQUIRE(readLe16(&out[1]) == -32768);
    REQUIRE(readLe16(&out[3]) == 32767);
    REQUIRE(readLe16(&out[7]) == -32768);
    REQUIRE(readLe16(&out[9]) == 32767);
    REQUIRE(readLe16(&out[11]) == -1);
}

TEST_CASE("encodeMotionPayload handles uint32 max delta", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(0, 0, 0, 0, 0, 0, 0, 0xFFFFFFFFU);
    REQUIRE(readLe32(&out[13]) == 0xFFFFFFFFU);
}

TEST_CASE("encodeMotionPayload zero-delta encodes as four zero bytes", "[motion]") {
    // First-packet sentinel: the receiver expects exactly 0, not 0xFFFFFFFF or
    // any other "no value" marker.
    const auto out = SatelliteClient::encodeMotionPayload(0, 0, 0, 0, 0, 0, 0, 0);
    REQUIRE(out[13] == 0);
    REQUIRE(out[14] == 0);
    REQUIRE(out[15] == 0);
    REQUIRE(out[16] == 0);
}

TEST_CASE("encodeBatteryPayload writes ctrlIdx + level + status in order", "[battery]") {
    const auto out = SatelliteClient::encodeBatteryPayload(/*ctrlIdx=*/5, /*level=*/77,
                                                           /*status=*/2);
    REQUIRE(out.size() == 3U);
    REQUIRE(out[0] == 5U);
    REQUIRE(out[1] == 77U);
    REQUIRE(out[2] == 2U);
}

TEST_CASE("encodeBatteryPayload preserves 0xFF (unknown) sentinel", "[battery]") {
    const auto out = SatelliteClient::encodeBatteryPayload(0, 0xFF, 0);
    REQUIRE(out[1] == 0xFFU);
    REQUIRE(out[2] == 0U);
}

TEST_CASE("MSG constants match the wire-protocol spec (types.h)", "[constants]") {
    // Wire contract with the receiver: bumping a value breaks every paired
    // client until it re-syncs.
    REQUIRE(SatelliteClient::kMsgMotion == 0x000A);
    REQUIRE(SatelliteClient::kMsgBattery == 0x000B);
    REQUIRE(SatelliteClient::kCapMotion == 0x0004);
    REQUIRE(SatelliteClient::kBatteryLevelUnknown == 0xFF);
    REQUIRE(SatelliteClient::kBatteryStatusUnknown == 0);
    REQUIRE(SatelliteClient::kBatteryStatusDischarging == 1);
    REQUIRE(SatelliteClient::kBatteryStatusCharging == 2);
    REQUIRE(SatelliteClient::kBatteryStatusFull == 3);
    REQUIRE(SatelliteClient::kBatteryStatusWired == 4);
}

// CAP_MOTION drives the web UI's motionCapable flag, so a pad with no IMU (an
// Xbox pad) must not advertise it even though the receiver accepts best-effort
// motion either way.

TEST_CASE("withMotionCapability sets bit 0x0004 iff the pad has an IMU", "[motion][caps]") {
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble;
    REQUIRE(base == 0x0003);

    SECTION("controller has an IMU -> CAP_MOTION is OR-ed in") {
        const std::uint16_t caps = SatelliteClient::withMotionCapability(base, true);
        REQUIRE(caps == 0x0007);
        REQUIRE((caps & SatelliteClient::kCapMotion) != 0);
    }
    SECTION("controller has no IMU -> word is unchanged") {
        const std::uint16_t caps = SatelliteClient::withMotionCapability(base, false);
        REQUIRE(caps == 0x0003);
        REQUIRE((caps & SatelliteClient::kCapMotion) == 0);
    }
}

TEST_CASE("withMotionCapability leaves the other capability bits intact", "[motion][caps]") {
    for (std::uint16_t base : {std::uint16_t{0x0000}, std::uint16_t{0x0003}, std::uint16_t{0x0008},
                               std::uint16_t{0xFFFB}}) {
        const std::uint16_t with = SatelliteClient::withMotionCapability(base, true);
        const std::uint16_t without = SatelliteClient::withMotionCapability(base, false);
        REQUIRE(without == base);
        REQUIRE((with & ~static_cast<std::uint16_t>(0x0004)) == base);
        REQUIRE((with | static_cast<std::uint16_t>(0x0004)) == with);
    }
}

TEST_CASE("withMotionCapability is idempotent when the bit is already set", "[motion][caps]") {
    const std::uint16_t base = 0x0003 | SatelliteClient::kCapMotion;
    REQUIRE(SatelliteClient::withMotionCapability(base, true) == base);
    REQUIRE(SatelliteClient::withMotionCapability(base, false) == base);
}

TEST_CASE("withRumbleCapability sets bit 0x0002 iff the slot's path can rumble", "[caps]") {
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers;
    REQUIRE(base == 0x0001);
    SECTION("SDL probe says the motors are drivable -> CAP_RUMBLE is OR-ed in") {
        const std::uint16_t caps = SatelliteClient::withRumbleCapability(base, true);
        REQUIRE(caps == 0x0003);
        REQUIRE((caps & SatelliteClient::kCapRumble) != 0);
    }
    SECTION("a USB-direct claim (no output write path) -> word is unchanged") {
        const std::uint16_t caps = SatelliteClient::withRumbleCapability(base, false);
        REQUIRE(caps == 0x0001);
        REQUIRE((caps & SatelliteClient::kCapRumble) == 0);
    }
}

TEST_CASE("withMotionCapability and withLightbarCapability compose independently",
          "[motion][caps]") {
    // registerController folds both bits, so they must be orthogonal.
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble;
    SECTION("IMU only") {
        const std::uint16_t caps = SatelliteClient::withLightbarCapability(
            SatelliteClient::withMotionCapability(base, true), false);
        REQUIRE(caps == 0x0007); // 0x0003 | CAP_MOTION
    }
    SECTION("LED only") {
        const std::uint16_t caps = SatelliteClient::withLightbarCapability(
            SatelliteClient::withMotionCapability(base, false), true);
        REQUIRE(caps == 0x000B); // 0x0003 | CAP_LIGHTBAR
    }
    SECTION("both (DualSense)") {
        const std::uint16_t caps = SatelliteClient::withLightbarCapability(
            SatelliteClient::withMotionCapability(base, true), true);
        REQUIRE(caps == 0x000F); // 0x0003 | CAP_MOTION | CAP_LIGHTBAR
    }
}
