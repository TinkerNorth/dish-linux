// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pins dish-linux session crypto to the SAME interop vectors the satellite
// (tests/test_windows_platform.cpp), dish-windows (test_session_crypto.cpp) and
// dish-android (SessionCryptoTest) assert. Any drift on any end is a cross-end
// protocol break, not a refactor.

#include "Network/SessionCrypto.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace dish::wire;

namespace {
// pairingKey = 01 02 .. 20 — the shared interop key all ends use.
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
    REQUIRE_FALSE(verifyHmacProof(key.data(), "device-2", p1)); // wrong device id
    auto other = key;
    other[0] = static_cast<std::uint8_t>(other[0] ^ 0xFF);
    REQUIRE_FALSE(verifyHmacProof(other.data(), "device-1", p1)); // diverged key
    REQUIRE_FALSE(verifyHmacProof(key.data(), "device-1", ""));   // malformed hex
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
    REQUIRE(std::memcmp(k1, k2, kCryptoKeySize) == 0);         // deterministic
    REQUIRE(std::memcmp(k1, key.data(), kCryptoKeySize) != 0); // never the raw pairing key

    deriveSessionKey(key.data(), salt, 0x12345679U, k2);
    REQUIRE(std::memcmp(k1, k2, kCryptoKeySize) != 0); // token changes the key

    const std::uint8_t salt2[kSessionSaltSize] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x19};
    deriveSessionKey(key.data(), salt2, 0x12345678U, k2);
    REQUIRE(std::memcmp(k1, k2, kCryptoKeySize) != 0); // salt changes the key
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

    // Same key + counter, opposite direction → different ciphertext (no nonce reuse).
    std::uint8_t ct2[64];
    unsigned long long ct2Len = 0;
    REQUIRE(
        encryptPacket(key, kDirServerToClient, 1, 0xAABBCCDDU, plain, sizeof(plain), ct2, &ct2Len));
    REQUIRE(ct2Len == ctLen);
    REQUIRE(std::memcmp(ct, ct2, static_cast<std::size_t>(ctLen)) != 0);
}

TEST_CASE("the client-to-server send framing decrypts as the satellite expects", "[wire][crypto]") {
    // Reconstruct one packet the way SatelliteClient builds it: the FIRST send
    // uses counter 1 (NOT 0 — the pre-protocol-1 off-by-one), nonce direction
    // CLIENT_TO_SERVER, AAD = token(4 BE). The satellite decrypts the opposite
    // way; here we prove the bytes round-trip under those exact parameters.
    std::array<std::uint8_t, kCryptoKeySize> sessionKey{};
    sessionKey[0] = 0xAB; // arbitrary derived key
    const std::uint32_t token = 0x0007a1b2u;

    // A heartbeat inner frame: type(2 BE) | len(2 BE) | (empty payload).
    const std::uint8_t inner[4] = {0x00, 0x02, 0x00, 0x00};

    std::uint8_t ct[64];
    unsigned long long ctLen = 0;
    REQUIRE(encryptPacket(sessionKey.data(), kDirClientToServer, /*counter=*/1, token, inner,
                          sizeof(inner), ct, &ctLen));

    // The satellite decrypts with the client→server direction + the SAME
    // counter 1 + token AAD. Counter 0 (the old origin) must NOT decrypt.
    std::uint8_t pt[64];
    unsigned long long ptLen = 0;
    REQUIRE(decryptPacket(sessionKey.data(), kDirClientToServer, 1, token, ct,
                          static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE(ptLen == sizeof(inner));
    REQUIRE(std::memcmp(pt, inner, sizeof(inner)) == 0);

    // Off-by-one (counter 0) and a missing direction byte both fail auth.
    REQUIRE_FALSE(decryptPacket(sessionKey.data(), kDirClientToServer, 0, token, ct,
                                static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE_FALSE(decryptPacket(sessionKey.data(), kDirServerToClient, 1, token, ct,
                                static_cast<std::size_t>(ctLen), pt, &ptLen));
}
