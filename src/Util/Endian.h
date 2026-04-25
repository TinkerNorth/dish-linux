// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Tiny big-endian helpers used by the wire protocol. The Satellite header
// transmits sequence number / timestamp / nonce in network byte order.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dish::util {

inline void putU16Be(std::uint8_t* dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    dst[1] = static_cast<std::uint8_t>(v & 0xFFU);
}

inline void putU32Be(std::uint8_t* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>(v & 0xFFU);
}

inline void putU64Be(std::uint8_t* dst, std::uint64_t v) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::uint8_t>((v >> (56U - (8U * i))) & 0xFFU);
    }
}

inline std::uint16_t readU16Be(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[0]) << 8) |
                                      static_cast<std::uint16_t>(src[1]));
}

inline std::uint32_t readU32Be(const std::uint8_t* src) noexcept {
    return (static_cast<std::uint32_t>(src[0]) << 24) | (static_cast<std::uint32_t>(src[1]) << 16) |
           (static_cast<std::uint32_t>(src[2]) << 8) | static_cast<std::uint32_t>(src[3]);
}

inline std::uint64_t readU64Be(const std::uint8_t* src) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) { v = (v << 8) | static_cast<std::uint64_t>(src[i]); }
    return v;
}

} // namespace dish::util
