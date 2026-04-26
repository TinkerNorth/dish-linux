// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Hex.h"

#include <cctype>

namespace dish::util {

namespace {

constexpr char kHexChars[] = "0123456789abcdef";

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
    if (c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
    return -1;
}

} // namespace

std::string toHex(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[(2 * i) + 0] = kHexChars[(data[i] >> 4) & 0x0F];
        out[(2 * i) + 1] = kHexChars[data[i] & 0x0F];
    }
    return out;
}

std::string toHex(const std::vector<std::uint8_t>& bytes) {
    return toHex(bytes.data(), bytes.size());
}

std::optional<std::vector<std::uint8_t>> fromHex(std::string_view hex) {
    if ((hex.size() % 2U) != 0U) { return std::nullopt; }
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2U);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexDigit(hex[i]);
        const int lo = hexDigit(hex[i + 1]);
        if (hi < 0 || lo < 0) { return std::nullopt; }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace dish::util
