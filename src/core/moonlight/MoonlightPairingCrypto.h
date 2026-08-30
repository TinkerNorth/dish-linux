// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The OpenSSL-backed primitives the Moonlight pairing handshake needs: SHA-256,
// AES-128-ECB, RSA-SHA256 PKCS#1 v1.5 sign/verify, and self-signed client
// identity generation. Semantics ported from Wolf's MIT-licensed crypto module
// (games-on-whales/wolf, src/moonlight-protocol/crypto) so the two ends of the
// handshake agree byte-for-byte; see THIRD_PARTY.md.
//
// This is the one core module with a crypto-library dependency beside
// wire/SessionCrypto (libsodium): the Moonlight protocol fixes AES-128 and RSA
// X.509 certificates, which libsodium deliberately does not provide, so it
// links OpenSSL's libcrypto instead.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dish::mooncrypto {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::size_t kAesBlockSize = 16;
inline constexpr std::size_t kAesKeySize = 16;
inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kPairingSaltSize = 16;
inline constexpr std::size_t kPairingSecretSize = 16;
// 2048-bit RSA, so every pairing signature is exactly this long.
inline constexpr std::size_t kRsaSignatureSize = 256;

std::array<std::uint8_t, kSha256Size> sha256(const std::uint8_t* data, std::size_t len);

// CSPRNG fill; false only when the system entropy source fails.
bool randomBytes(std::uint8_t* out, std::size_t len);

// Pairing key = first 16 bytes of SHA-256(salt bytes || PIN as ASCII digits).
std::array<std::uint8_t, kAesKeySize>
derivePairingKey(const std::array<std::uint8_t, kPairingSaltSize>& salt, const std::string& pin);

// AES-128-ECB without padding: `len` must be a multiple of the block size.
// nullopt on a bad length or an OpenSSL failure.
std::optional<Bytes> aesEcbEncrypt(const std::array<std::uint8_t, kAesKeySize>& key,
                                   const std::uint8_t* data, std::size_t len);
std::optional<Bytes> aesEcbDecrypt(const std::array<std::uint8_t, kAesKeySize>& key,
                                   const std::uint8_t* data, std::size_t len);

// RSA PKCS#1 v1.5 over SHA-256, the signature scheme both pairing directions
// use. Sign takes the PEM private key; verify takes the peer's PEM certificate.
std::optional<Bytes> rsaSignSha256(const std::string& privateKeyPem, const std::uint8_t* msg,
                                   std::size_t len);
bool rsaVerifySha256(const std::string& certPem, const std::uint8_t* msg, std::size_t len,
                     const std::uint8_t* sig, std::size_t sigLen);

// The client identity: a 2048-bit RSA key and a self-signed X.509 certificate,
// generated once and persisted. The cert authenticates every HTTPS call after
// pairing, so losing it means re-pairing every host.
struct ClientIdentity {
    std::string certPem;
    std::string privateKeyPem;
};

std::optional<ClientIdentity> generateClientIdentity();

// The DER bytes of the certificate's signature BIT STRING — the "cert
// signature" material both pairing hashes mix in.
std::optional<Bytes> certSignature(const std::string& certPem);

// Lowercase hex SHA-256 of the certificate's DER encoding, for TLS pinning.
std::optional<std::string> certFingerprintHex(const std::string& certPem);

// True when `pem` parses as an X.509 certificate.
bool isValidCertPem(const std::string& pem);

} // namespace dish::mooncrypto
