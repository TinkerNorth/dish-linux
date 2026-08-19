// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The USB spec math behind the expected values. computeUsbPollRateHz: full-speed
// (maxPacket <= 64) = 1000/epInterval; high-speed (>= 65) = 8000/2^(epInterval-1),
// exponent clamped so an absurd interval cannot overflow the shift.
// measuredPollRateHz: floor(deltaCount/deltaMs * 1000).

#include "core/reducer/UsbPollRate.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::computeUsbPollRateHz;
using dish::reducer::measuredPollRateHz;

TEST_CASE("full-speed 1ms interval is 1000 Hz", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(1, 64) == 1000);
}

TEST_CASE("full-speed 8ms interval is 125 Hz", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(8, 64) == 125);
}

TEST_CASE("full-speed 10ms interval is 100 Hz with a small packet", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(10, 20) == 100);
}

TEST_CASE("high-speed exponent 1 is 8000 Hz", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(1, 65) == 8000);
}

TEST_CASE("high-speed exponent 4 is 1000 Hz", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(4, 512) == 1000);
}

TEST_CASE("high-speed exponent 2 is 4000 Hz", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(2, 512) == 4000);
}

TEST_CASE("high-speed large max packet still decodes the exponent", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(1, 1024) == 8000);
}

TEST_CASE("packet size of exactly 64 is treated as full-speed", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(1, 64) == 1000);
}

TEST_CASE("packet size of 65 crosses into high-speed", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(1, 65) == 8000);
}

TEST_CASE("zero interval yields zero", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(0, 64) == 0);
}

TEST_CASE("negative interval yields zero", "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(-5, 64) == 0);
}

TEST_CASE("high-speed exponent is clamped so an absurd interval cannot overflow the shift",
          "[usb-pollrate]") {
    CHECK(computeUsbPollRateHz(16, 128) == 0);
}

TEST_CASE("500 completions over 500 ms is 1000 Hz", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(500, 500) == 1000);
}

TEST_CASE("125 completions over 1000 ms is 125 Hz", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(125, 1000) == 125);
}

TEST_CASE("1000 completions over 500 ms is 2000 Hz", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(1000, 500) == 2000);
}

TEST_CASE("no completions reports zero rather than the previous reading", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(0, 500) == 0);
}

TEST_CASE("zero elapsed window yields zero", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(500, 0) == 0);
}

TEST_CASE("negative elapsed window yields zero", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(500, -10) == 0);
}

TEST_CASE("negative count delta from a counter reset yields zero not a negative rate",
          "[usb-pollrate]") {
    CHECK(measuredPollRateHz(-995, 500) == 0);
}

TEST_CASE("rate is integer-floored", "[usb-pollrate]") {
    CHECK(measuredPollRateHz(3, 2) == 1500);
    CHECK(measuredPollRateHz(1, 3) == 333);
}
