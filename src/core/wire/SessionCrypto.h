// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Session-key derivation, REST proof-of-key-possession and UDP packet AEAD.
// Byte-for-byte identical to the satellite's net/session_crypto.* (see its
// docs/contract.md, Crypto); the interop vectors in tests/test_session_crypto.cpp
// are shared with the satellite and Android ends, so any drift breaks all three.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dish::wire {

inline constexpr std::size_t kCryptoKeySize = 32;
inline constexpr std::size_t kSessionSaltSize = 8;
inline constexpr std::size_t kCryptoNonceSize = 12;

// Nonce direction byte (nonce[0]); keeps the two directions of one session key
// from ever sharing a nonce. See satellite/docs/contract.md §Crypto.
inline constexpr std::uint8_t kDirClientToServer = 0x00;
inline constexpr std::uint8_t kDirServerToClient = 0x01;

// sessionKey = HKDF-SHA256(ikm = pairingKey, salt = sessionSalt,
//                          info = "satellite-session-v1" || token(4 BE)).
// RFC 5869 extract-then-expand, single 32-byte output block. Both ends derive
// the same key from the session PUT response's token + sessionSalt, so counters
// restart at 1 with no cross-session nonce reuse.
void deriveSessionKey(const std::uint8_t pairingKey[kCryptoKeySize],
                      const std::uint8_t sessionSalt[kSessionSaltSize], std::uint32_t token,
                      std::uint8_t outSessionKey[kCryptoKeySize]);

// hex( HMAC-SHA256( pairingKey, "satellite-proof:" + deviceId ) ), lowercase.
// Sent in the X-Hmac-Proof header on every authenticated REST call.
std::string computeHmacProof(const std::uint8_t pairingKey[kCryptoKeySize],
                             const std::string& deviceId);

// Constant-time verify of a hex proof against (pairingKey, deviceId). False on
// malformed hex. Symmetric with the satellite's server-side check.
bool verifyHmacProof(const std::uint8_t pairingKey[kCryptoKeySize], const std::string& deviceId,
                     const std::string& proofHex);

// UDP packet AEAD (ChaCha20-Poly1305-IETF). nonce = direction(1) | 0x00×7 |
// counter(4 BE); AAD = token(4 BE). `ciphertext` must have room for
// ptLen + the 16-byte Poly1305 tag.
bool encryptPacket(const std::uint8_t key[kCryptoKeySize], std::uint8_t direction,
                   std::uint32_t counter, std::uint32_t token, const std::uint8_t* plaintext,
                   std::size_t ptLen, std::uint8_t* ciphertext, unsigned long long* ctLen);
bool decryptPacket(const std::uint8_t key[kCryptoKeySize], std::uint8_t direction,
                   std::uint32_t counter, std::uint32_t token, const std::uint8_t* ciphertext,
                   std::size_t ctLen, std::uint8_t* plaintext, unsigned long long* ptLen);

} // namespace dish::wire
