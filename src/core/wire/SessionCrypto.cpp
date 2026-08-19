// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/wire/SessionCrypto.h"

#include <sodium.h>

#include <cstring>

namespace dish::wire {
namespace {

void hmacSha256(const std::uint8_t* key, std::size_t keyLen, const std::uint8_t* msg,
                std::size_t msgLen, std::uint8_t out[crypto_auth_hmacsha256_BYTES]) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key, keyLen);
    crypto_auth_hmacsha256_update(&st, msg, msgLen);
    crypto_auth_hmacsha256_final(&st, out);
}

int hexNibbleAt(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

void buildNonceAndAad(std::uint8_t direction, std::uint32_t counter, std::uint32_t token,
                      std::uint8_t nonce[kCryptoNonceSize], std::uint8_t aad[4]) {
    std::memset(nonce, 0, kCryptoNonceSize);
    nonce[0] = direction;
    nonce[8] = static_cast<std::uint8_t>(counter >> 24);
    nonce[9] = static_cast<std::uint8_t>(counter >> 16);
    nonce[10] = static_cast<std::uint8_t>(counter >> 8);
    nonce[11] = static_cast<std::uint8_t>(counter);
    aad[0] = static_cast<std::uint8_t>(token >> 24);
    aad[1] = static_cast<std::uint8_t>(token >> 16);
    aad[2] = static_cast<std::uint8_t>(token >> 8);
    aad[3] = static_cast<std::uint8_t>(token);
}

} // namespace

void deriveSessionKey(const std::uint8_t pairingKey[kCryptoKeySize],
                      const std::uint8_t sessionSalt[kSessionSaltSize], std::uint32_t token,
                      std::uint8_t outSessionKey[kCryptoKeySize]) {
    // RFC 5869: PRK = HMAC(salt, IKM); OKM(32) = T1 = HMAC(PRK, info || 0x01).
    std::uint8_t prk[crypto_auth_hmacsha256_BYTES];
    hmacSha256(sessionSalt, kSessionSaltSize, pairingKey, kCryptoKeySize, prk);

    static const char infoLabel[] = "satellite-session-v1";
    constexpr std::size_t kLabelLen = sizeof(infoLabel) - 1; // 20, no trailing NUL
    std::uint8_t info[kLabelLen + 4 + 1];
    std::memcpy(info, infoLabel, kLabelLen);
    info[kLabelLen + 0] = static_cast<std::uint8_t>(token >> 24);
    info[kLabelLen + 1] = static_cast<std::uint8_t>(token >> 16);
    info[kLabelLen + 2] = static_cast<std::uint8_t>(token >> 8);
    info[kLabelLen + 3] = static_cast<std::uint8_t>(token);
    info[kLabelLen + 4] = 0x01;

    std::uint8_t t1[crypto_auth_hmacsha256_BYTES];
    hmacSha256(prk, sizeof(prk), info, sizeof(info), t1);
    static_assert(sizeof(t1) == kCryptoKeySize, "one HKDF block fills the session key");
    std::memcpy(outSessionKey, t1, kCryptoKeySize);
    sodium_memzero(prk, sizeof(prk));
    sodium_memzero(t1, sizeof(t1));
}

std::string computeHmacProof(const std::uint8_t pairingKey[kCryptoKeySize],
                             const std::string& deviceId) {
    const std::string msg = "satellite-proof:" + deviceId;
    std::uint8_t mac[crypto_auth_hmacsha256_BYTES];
    hmacSha256(pairingKey, kCryptoKeySize, reinterpret_cast<const std::uint8_t*>(msg.data()),
               msg.size(), mac);
    char hex[crypto_auth_hmacsha256_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), mac, sizeof(mac));
    sodium_memzero(mac, sizeof(mac));
    return std::string(hex);
}

bool verifyHmacProof(const std::uint8_t pairingKey[kCryptoKeySize], const std::string& deviceId,
                     const std::string& proofHex) {
    if (proofHex.size() != static_cast<std::size_t>(crypto_auth_hmacsha256_BYTES) * 2u) {
        return false;
    }
    std::uint8_t supplied[crypto_auth_hmacsha256_BYTES];
    for (std::size_t i = 0; i < sizeof(supplied); ++i) {
        const int hi = hexNibbleAt(proofHex[i * 2]);
        const int lo = hexNibbleAt(proofHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { return false; }
        supplied[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    const std::string msg = "satellite-proof:" + deviceId;
    std::uint8_t expected[crypto_auth_hmacsha256_BYTES];
    hmacSha256(pairingKey, kCryptoKeySize, reinterpret_cast<const std::uint8_t*>(msg.data()),
               msg.size(), expected);
    const bool ok = sodium_memcmp(supplied, expected, sizeof(expected)) == 0;
    sodium_memzero(expected, sizeof(expected));
    return ok;
}

bool encryptPacket(const std::uint8_t key[kCryptoKeySize], std::uint8_t direction,
                   std::uint32_t counter, std::uint32_t token, const std::uint8_t* plaintext,
                   std::size_t ptLen, std::uint8_t* ciphertext, unsigned long long* ctLen) {
    std::uint8_t nonce[kCryptoNonceSize];
    std::uint8_t aad[4];
    buildNonceAndAad(direction, counter, token, nonce, aad);
    return crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext, ctLen, plaintext, ptLen, aad,
                                                     sizeof(aad), nullptr, nonce, key) == 0;
}

bool decryptPacket(const std::uint8_t key[kCryptoKeySize], std::uint8_t direction,
                   std::uint32_t counter, std::uint32_t token, const std::uint8_t* ciphertext,
                   std::size_t ctLen, std::uint8_t* plaintext, unsigned long long* ptLen) {
    std::uint8_t nonce[kCryptoNonceSize];
    std::uint8_t aad[4];
    buildNonceAndAad(direction, counter, token, nonce, aad);
    return crypto_aead_chacha20poly1305_ietf_decrypt(plaintext, ptLen, nullptr, ciphertext, ctLen,
                                                     aad, sizeof(aad), nonce, key) == 0;
}

} // namespace dish::wire
