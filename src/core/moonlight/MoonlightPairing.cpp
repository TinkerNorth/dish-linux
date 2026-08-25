// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/moonlight/MoonlightPairing.h"

#include "Util/Hex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace dish::moonpair {
namespace {

using mooncrypto::Bytes;

// The wire convention is uppercase hex; every parser on the host side accepts
// either case, but emitting what real clients emit costs nothing.
std::string toUpperHex(const std::uint8_t* data, std::size_t len) {
    std::string hex = util::toHex(data, len);
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return hex;
}

std::optional<Bytes> fromHex(const std::string& hex) { return util::fromHex(hex); }

} // namespace

std::string pinFromRandom(std::uint32_t random) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%04u", random % 10000U);
    return buf;
}

PairingSession::PairingSession(std::string clientCertPem, std::string clientKeyPem,
                               const std::array<std::uint8_t, 16>& salt, const std::string& pin,
                               const std::array<std::uint8_t, 16>& clientChallenge,
                               const std::array<std::uint8_t, 16>& clientSecret)
    : clientCertPem_(std::move(clientCertPem)), clientKeyPem_(std::move(clientKeyPem)), salt_(salt),
      aesKey_(mooncrypto::derivePairingKey(salt, pin)), clientChallenge_(clientChallenge),
      clientSecret_(clientSecret) {}

std::string PairingSession::saltHex() const { return toUpperHex(salt_.data(), salt_.size()); }

std::string PairingSession::clientCertHex() const {
    return toUpperHex(reinterpret_cast<const std::uint8_t*>(clientCertPem_.data()),
                      clientCertPem_.size());
}

bool PairingSession::acceptServerCert(const std::string& plaincertHex) {
    const auto pemBytes = fromHex(plaincertHex);
    if (!pemBytes || pemBytes->empty()) { return false; }
    std::string pem(pemBytes->begin(), pemBytes->end());
    if (!mooncrypto::isValidCertPem(pem)) { return false; }
    serverCertPem_ = std::move(pem);
    return true;
}

std::optional<std::string> PairingSession::clientChallengeHex() const {
    if (serverCertPem_.empty()) { return std::nullopt; }
    const auto encrypted =
        mooncrypto::aesEcbEncrypt(aesKey_, clientChallenge_.data(), clientChallenge_.size());
    if (!encrypted) { return std::nullopt; }
    return toUpperHex(encrypted->data(), encrypted->size());
}

std::optional<std::string>
PairingSession::acceptChallengeResponse(const std::string& challengeResponseHex) {
    const auto encrypted = fromHex(challengeResponseHex);
    if (!encrypted) { return std::nullopt; }
    const auto decrypted = mooncrypto::aesEcbDecrypt(aesKey_, encrypted->data(), encrypted->size());
    // hash(32) + server challenge(16).
    if (!decrypted || decrypted->size() < mooncrypto::kSha256Size + serverChallenge_.size()) {
        return std::nullopt;
    }
    std::memcpy(serverResponseHash_.data(), decrypted->data(), serverResponseHash_.size());
    std::memcpy(serverChallenge_.data(), decrypted->data() + mooncrypto::kSha256Size,
                serverChallenge_.size());
    haveChallengeResponse_ = true;

    // Client hash = SHA-256(server challenge + client cert signature + client
    // secret), sent back encrypted.
    const auto certSig = mooncrypto::certSignature(clientCertPem_);
    if (!certSig) { return std::nullopt; }
    Bytes material(serverChallenge_.begin(), serverChallenge_.end());
    material.insert(material.end(), certSig->begin(), certSig->end());
    material.insert(material.end(), clientSecret_.begin(), clientSecret_.end());
    const auto clientHash = mooncrypto::sha256(material.data(), material.size());
    const auto sealed = mooncrypto::aesEcbEncrypt(aesKey_, clientHash.data(), clientHash.size());
    if (!sealed) { return std::nullopt; }
    return toUpperHex(sealed->data(), sealed->size());
}

bool PairingSession::acceptPairingSecret(const std::string& pairingSecretHex) {
    if (!haveChallengeResponse_ || serverCertPem_.empty()) { return false; }
    const auto secretAndSig = fromHex(pairingSecretHex);
    if (!secretAndSig ||
        secretAndSig->size() < mooncrypto::kPairingSecretSize + mooncrypto::kRsaSignatureSize) {
        return false;
    }
    const std::uint8_t* serverSecret = secretAndSig->data();
    const std::uint8_t* signature = secretAndSig->data() + mooncrypto::kPairingSecretSize;

    // The phase-2 hash must commit to OUR challenge, the server cert's own
    // signature bytes and the secret the server just revealed. A wrong PIN
    // breaks this (the decryptions diverge), as does a substituted cert.
    const auto serverCertSig = mooncrypto::certSignature(serverCertPem_);
    if (!serverCertSig) { return false; }
    Bytes material(clientChallenge_.begin(), clientChallenge_.end());
    material.insert(material.end(), serverCertSig->begin(), serverCertSig->end());
    material.insert(material.end(), serverSecret, serverSecret + mooncrypto::kPairingSecretSize);
    const auto expected = mooncrypto::sha256(material.data(), material.size());
    if (std::memcmp(expected.data(), serverResponseHash_.data(), expected.size()) != 0) {
        return false;
    }

    // And the secret must be signed by the key behind the server certificate.
    return mooncrypto::rsaVerifySha256(serverCertPem_, serverSecret, mooncrypto::kPairingSecretSize,
                                       signature, mooncrypto::kRsaSignatureSize);
}

std::optional<std::string> PairingSession::clientPairingSecretHex() const {
    const auto signature =
        mooncrypto::rsaSignSha256(clientKeyPem_, clientSecret_.data(), clientSecret_.size());
    if (!signature) { return std::nullopt; }
    Bytes payload(clientSecret_.begin(), clientSecret_.end());
    payload.insert(payload.end(), signature->begin(), signature->end());
    return toUpperHex(payload.data(), payload.size());
}

} // namespace dish::moonpair
