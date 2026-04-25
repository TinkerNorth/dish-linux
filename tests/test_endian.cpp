// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/Endian.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace dish::util;

TEST_CASE("putU16Be writes big-endian bytes", "[endian]") {
    std::array<std::uint8_t, 2> buf{};
    putU16Be(buf.data(), 0xBEEF);
    REQUIRE(buf[0] == 0xBE);
    REQUIRE(buf[1] == 0xEF);
}

TEST_CASE("putU32Be writes big-endian bytes", "[endian]") {
    std::array<std::uint8_t, 4> buf{};
    putU32Be(buf.data(), 0xDEADBEEFU);
    REQUIRE(buf[0] == 0xDE);
    REQUIRE(buf[1] == 0xAD);
    REQUIRE(buf[2] == 0xBE);
    REQUIRE(buf[3] == 0xEF);
}

TEST_CASE("putU64Be writes big-endian bytes", "[endian]") {
    std::array<std::uint8_t, 8> buf{};
    putU64Be(buf.data(), 0x0123456789ABCDEFULL);
    const std::array<std::uint8_t, 8> expected{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    REQUIRE(buf == expected);
}

TEST_CASE("readU16Be / readU32Be / readU64Be round-trip put*Be", "[endian]") {
    std::array<std::uint8_t, 8> buf{};
    putU64Be(buf.data(), 0xCAFEBABEDEADBEEFULL);
    REQUIRE(readU64Be(buf.data()) == 0xCAFEBABEDEADBEEFULL);

    putU32Be(buf.data(), 0x11223344U);
    REQUIRE(readU32Be(buf.data()) == 0x11223344U);

    putU16Be(buf.data(), 0xAABB);
    REQUIRE(readU16Be(buf.data()) == 0xAABB);
}
