// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/Hex.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::util::fromHex;
using dish::util::toHex;

TEST_CASE("toHex emits lowercase hex with leading zeros", "[hex]") {
    const std::vector<std::uint8_t> bytes{0x00, 0x0F, 0xA1, 0xFF};
    REQUIRE(toHex(bytes) == "000fa1ff");
}

TEST_CASE("toHex of an empty buffer yields an empty string", "[hex]") {
    REQUIRE(toHex(std::vector<std::uint8_t>{}).empty());
}

TEST_CASE("fromHex round-trips toHex", "[hex]") {
    const std::vector<std::uint8_t> bytes{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};
    const auto encoded = toHex(bytes);
    const auto decoded = fromHex(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == bytes);
}

TEST_CASE("fromHex accepts upper- and mixed-case input", "[hex]") {
    const auto a = fromHex("DEADBEEF");
    const auto b = fromHex("DeAdBeEf");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a == *b);
}

TEST_CASE("fromHex rejects odd-length and non-hex input", "[hex]") {
    REQUIRE_FALSE(fromHex("abc").has_value());
    REQUIRE_FALSE(fromHex("zz").has_value());
    REQUIRE_FALSE(fromHex("12 34").has_value());
}
