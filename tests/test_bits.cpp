// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/Bits.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace bits = dish::bits;

TEST_CASE("bits: a byte widens to an unsigned word that keeps its top bit", "[bits]") {
    CHECK(bits::u32(std::uint8_t{0x80}) << 24U == 0x80000000U);
    CHECK(bits::u32(std::uint8_t{0xFF}) << 24U == 0xFF000000U);
    CHECK(bits::u32(std::uint16_t{0x8000}) << 16U == 0x80000000U);
    CHECK(bits::u64(std::uint8_t{0x80}) << 56U == 0x8000000000000000ULL);
}

TEST_CASE("bits: loads assemble every boundary byte in every position", "[bits]") {
    const std::array<std::uint8_t, 4> b{0x80, 0xFF, 0x7F, 0x00};
    CHECK(bits::loadLe16(b.data()) == 0xFF80U);
    CHECK(bits::loadBe16(b.data()) == 0x80FFU);
    CHECK(bits::loadLe32(b.data()) == 0x007FFF80U);
    CHECK(bits::loadBe32(b.data()) == 0x80FF7F00U);
    const std::array<std::uint8_t, 4> top{0x00, 0x00, 0x00, 0x80};
    CHECK(bits::loadLe32(top.data()) == 0x80000000U);
}

TEST_CASE("bits: stores split a word into its bytes, top bit included", "[bits]") {
    std::array<std::uint8_t, 4> out{};
    bits::storeLe32(out.data(), 0x80FF7F01U);
    CHECK(out == std::array<std::uint8_t, 4>{0x01, 0x7F, 0xFF, 0x80});
    bits::storeBe32(out.data(), 0x80FF7F01U);
    CHECK(out == std::array<std::uint8_t, 4>{0x80, 0xFF, 0x7F, 0x01});
    std::array<std::uint8_t, 2> half{};
    bits::storeLe16(half.data(), 0x80FFU);
    CHECK(half == std::array<std::uint8_t, 2>{0xFF, 0x80});
    bits::storeBe16(half.data(), 0x80FFU);
    CHECK(half == std::array<std::uint8_t, 2>{0x80, 0xFF});
}

TEST_CASE("bits: the signed view is the two's-complement reading", "[bits]") {
    CHECK(bits::asInt16(0x0000U) == 0);
    CHECK(bits::asInt16(0x7FFFU) == 32767);
    CHECK(bits::asInt16(0x8000U) == -32768);
    CHECK(bits::asInt16(0xFFFFU) == -1);
    CHECK(bits::asInt32(0x80000000U) == INT32_MIN);
    CHECK(bits::asInt32(0xFFFFFFFFU) == -1);
    CHECK(bits::asUint16(-1) == 0xFFFFU);
    CHECK(bits::asUint32(-2) == 0xFFFFFFFEU);
}

TEST_CASE("bits: narrowing keeps the low part only", "[bits]") {
    CHECK(bits::lowByte(0x12345680U) == 0x80U);
    CHECK(bits::low16(0x1234FF80U) == 0xFF80U);
    CHECK(bits::byteOf(0x80FF7F01U, 3U) == 0x80U);
    CHECK(bits::byteOf(std::uint64_t{0xFF00000000000000ULL}, 7U) == 0xFFU);
}

TEST_CASE("bits: pow2 is the doubling a shift used to spell", "[bits]") {
    CHECK(bits::pow2(0U) == 1);
    CHECK(bits::pow2(6U) == 64);
    CHECK(bits::pow2(15U) == 32768);
    CHECK(bits::pow2(62U) == (std::int64_t{1} << 62));
}
