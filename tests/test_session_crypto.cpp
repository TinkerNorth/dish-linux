// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The vectors below are the ones the satellite and dish-android assert too, so
// a change here is a cross-end protocol break, never a refactor.

#include "core/wire/SessionCrypto.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace dish::wire;

namespace {
// pairingKey = 01 02 .. 20, the shared interop key all three ends use.
std::array<std::uint8_t, kCryptoKeySize> interopKey() {
    std::array<std::uint8_t, kCryptoKeySize> k{};
    for (std::size_t i = 0; i < k.size(); ++i) { k[i] = static_cast<std::uint8_t>(i + 1); }
    return k;
}

std::string toHexLower(const std::uint8_t* p, std::size_t n) {
    static const char* const digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 0x0F]);
    }
    return s;
}
} // namespace

TEST_CASE("computeHmacProof matches the pinned interop vector", "[wire][crypto]") {
    const auto key = interopKey();
    REQUIRE(computeHmacProof(key.data(), "device-1") ==
            "05a035a10c55fdfe254c9df5df55a614ac128b123a5de225ea33b41f1d4eedde");
    REQUIRE(computeHmacProof(key.data(), "device-1").size() == 64U);
}

TEST_CASE("computeHmacProof is device-bound and key-bound", "[wire][crypto]") {
    const auto key = interopKey();
    const auto p1 = computeHmacProof(key.data(), "device-1");
    REQUIRE(computeHmacProof(key.data(), "device-2") != p1);
    auto other = key;
    other[0] = 0x7F;
    REQUIRE(computeHmacProof(other.data(), "device-1") != p1);
}

TEST_CASE("verifyHmacProof accepts a matching proof and rejects tampering", "[wire][crypto]") {
    const auto key = interopKey();
    const auto p1 = computeHmacProof(key.data(), "device-1");
    REQUIRE(verifyHmacProof(key.data(), "device-1", p1));
    REQUIRE_FALSE(verifyHmacProof(key.data(), "device-2", p1));
    auto other = key;
    other[0] = static_cast<std::uint8_t>(other[0] ^ 0xFF);
    REQUIRE_FALSE(verifyHmacProof(other.data(), "device-1", p1));
    REQUIRE_FALSE(verifyHmacProof(key.data(), "device-1", "")); // malformed hex
    REQUIRE_FALSE(verifyHmacProof(key.data(), "device-1", "abc"));
    auto bad = p1;
    bad[0] = 'z';
    REQUIRE_FALSE(verifyHmacProof(key.data(), "device-1", bad));
}

TEST_CASE("deriveSessionKey matches the pinned HKDF interop vector", "[wire][crypto]") {
    const auto key = interopKey();
    const std::uint8_t salt[kSessionSaltSize] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18};
    std::uint8_t out[kCryptoKeySize];
    deriveSessionKey(key.data(), salt, 0x12345678U, out);
    REQUIRE(toHexLower(out, kCryptoKeySize) ==
            "946f704cf07e2dde5e9995a70d3d103753b4687a7ed9656bc6481b06065a8584");
}

TEST_CASE("deriveSessionKey is deterministic, never the raw key, varies with inputs",
          "[wire][crypto]") {
    const auto key = interopKey();
    const std::uint8_t salt[kSessionSaltSize] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18};
    std::uint8_t k1[kCryptoKeySize];
    std::uint8_t k2[kCryptoKeySize];
    deriveSessionKey(key.data(), salt, 0x12345678U, k1);
    deriveSessionKey(key.data(), salt, 0x12345678U, k2);
    REQUIRE(std::memcmp(k1, k2, kCryptoKeySize) == 0);
    REQUIRE(std::memcmp(k1, key.data(), kCryptoKeySize) != 0); // never the raw pairing key

    deriveSessionKey(key.data(), salt, 0x12345679U, k2);
    REQUIRE(std::memcmp(k1, k2, kCryptoKeySize) != 0); // the token changes the key

    const std::uint8_t salt2[kSessionSaltSize] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x19};
    deriveSessionKey(key.data(), salt2, 0x12345678U, k2);
    REQUIRE(std::memcmp(k1, k2, kCryptoKeySize) != 0); // the salt changes the key
}

TEST_CASE("packet AEAD round-trips and the direction byte is bound into the nonce",
          "[wire][crypto]") {
    std::uint8_t key[kCryptoKeySize] = {9, 9, 9};          // remaining bytes zero
    const std::uint8_t plain[] = {0x00, 0x02, 0x00, 0x00}; // a heartbeat inner
    std::uint8_t ct[64];
    unsigned long long ctLen = 0;
    REQUIRE(
        encryptPacket(key, kDirClientToServer, 1, 0xAABBCCDDU, plain, sizeof(plain), ct, &ctLen));

    std::uint8_t pt[64];
    unsigned long long ptLen = 0;
    REQUIRE(decryptPacket(key, kDirClientToServer, 1, 0xAABBCCDDU, ct,
                          static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE(ptLen == sizeof(plain));
    REQUIRE(std::memcmp(pt, plain, sizeof(plain)) == 0);

    // Direction / counter / token mismatch each fail authentication.
    REQUIRE_FALSE(decryptPacket(key, kDirServerToClient, 1, 0xAABBCCDDU, ct,
                                static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE_FALSE(decryptPacket(key, kDirClientToServer, 2, 0xAABBCCDDU, ct,
                                static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE_FALSE(decryptPacket(key, kDirClientToServer, 1, 0xAABBCCDEU, ct,
                                static_cast<std::size_t>(ctLen), pt, &ptLen));

    // Same key + counter, opposite direction: different ciphertext, no nonce reuse.
    std::uint8_t ct2[64];
    unsigned long long ct2Len = 0;
    REQUIRE(
        encryptPacket(key, kDirServerToClient, 1, 0xAABBCCDDU, plain, sizeof(plain), ct2, &ct2Len));
    REQUIRE(ct2Len == ctLen);
    REQUIRE(std::memcmp(ct, ct2, static_cast<std::size_t>(ctLen)) != 0);
}
