// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/net/Tofu.h"

#include <sodium.h>

#include <array>

namespace dish::net {

namespace {

// ASCII-only: fingerprint comparison must not depend on the locale.
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCaseAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) { return false; }
    }
    return true;
}

} // namespace

TofuVerdict tofuVerdict(const std::optional<std::string>& stored, const std::string& presented) {
    if (!stored.has_value()) { return TofuVerdict::TrustFirstUse; }
    if (equalsIgnoreCaseAscii(*stored, presented)) { return TofuVerdict::Match; }
    return TofuVerdict::Mismatch;
}

std::string sha256FingerprintHex(const std::uint8_t* bytes, std::size_t len) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), bytes, static_cast<unsigned long long>(len));

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const unsigned char b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

} // namespace dish::net
