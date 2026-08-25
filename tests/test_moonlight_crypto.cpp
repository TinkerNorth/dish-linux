// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pairing primitives against published vectors (FIPS-197 for AES-128-ECB,
// the classic "abc" SHA-256 vector), a pinned key-derivation vector, and
// generate/sign/verify round-trips over real 2048-bit identities.

#include "core/moonlight/MoonlightPairingCrypto.h"

#include "Util/Hex.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace dish::mooncrypto;
namespace util = dish::util;

namespace {

std::array<std::uint8_t, 16> key16(const std::string& hex) {
    const auto bytes = util::fromHex(hex);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == 16);
    std::array<std::uint8_t, 16> out{};
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return out;
}

// One identity per suite run; RSA keygen is the slow part.
const ClientIdentity& testIdentity() {
    static const ClientIdentity identity = [] {
        const auto id = generateClientIdentity();
        REQUIRE(id.has_value());
        return *id;
    }();
    return identity;
}

} // namespace

TEST_CASE("sha256 matches the published vector", "[moonlight][crypto]") {
    const std::string msg = "abc";
    const auto digest = sha256(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
    CHECK(util::toHex(digest.data(), digest.size()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("AES-128-ECB matches the FIPS-197 vector", "[moonlight][crypto]") {
    const auto key = key16("000102030405060708090a0b0c0d0e0f");
    const auto plaintext = util::fromHex("00112233445566778899aabbccddeeff");
    REQUIRE(plaintext.has_value());

    const auto encrypted = aesEcbEncrypt(key, plaintext->data(), plaintext->size());
    REQUIRE(encrypted.has_value());
    CHECK(util::toHex(*encrypted) == "69c4e0d86a7b0430d8cdb78070b4c55a");

    const auto decrypted = aesEcbDecrypt(key, encrypted->data(), encrypted->size());
    REQUIRE(decrypted.has_value());
    CHECK(util::toHex(*decrypted) == "00112233445566778899aabbccddeeff");
}

TEST_CASE("AES-128-ECB handles multi-block input without padding", "[moonlight][crypto]") {
    const auto key = key16("edf04a215c4fbea20934120c8480d855");
    std::vector<std::uint8_t> plaintext(48, 0xAB); // hash(32) + challenge(16)
    const auto encrypted = aesEcbEncrypt(key, plaintext.data(), plaintext.size());
    REQUIRE(encrypted.has_value());
    CHECK(encrypted->size() == 48);
    const auto decrypted = aesEcbDecrypt(key, encrypted->data(), encrypted->size());
    REQUIRE(decrypted.has_value());
    CHECK(*decrypted == plaintext);
}

TEST_CASE("AES-128-ECB rejects non-block-aligned input", "[moonlight][crypto]") {
    const auto key = key16("000102030405060708090a0b0c0d0e0f");
    const std::vector<std::uint8_t> odd(15, 0x01);
    CHECK_FALSE(aesEcbEncrypt(key, odd.data(), odd.size()).has_value());
    CHECK_FALSE(aesEcbDecrypt(key, odd.data(), odd.size()).has_value());
    CHECK_FALSE(aesEcbEncrypt(key, odd.data(), 0).has_value());
}

TEST_CASE("pairing key = SHA-256(salt || PIN) truncated to 16 bytes", "[moonlight][crypto]") {
    std::array<std::uint8_t, kPairingSaltSize> salt{};
    for (std::size_t i = 0; i < salt.size(); ++i) { salt[i] = static_cast<std::uint8_t>(i); }
    const auto key = derivePairingKey(salt, "4989");
    // Pinned: sha256(00..0f || "4989")[0:16].
    CHECK(util::toHex(key.data(), key.size()) == "1e3644c22cb825f6944041deab6c5cfb");
}

TEST_CASE("generated client identity is a usable self-signed cert", "[moonlight][crypto]") {
    const auto& id = testIdentity();
    CHECK(id.certPem.find("BEGIN CERTIFICATE") != std::string::npos);
    CHECK(id.privateKeyPem.find("PRIVATE KEY") != std::string::npos);
    CHECK(isValidCertPem(id.certPem));

    const auto sig = certSignature(id.certPem);
    REQUIRE(sig.has_value());
    // 2048-bit RSA self-signature.
    CHECK(sig->size() == kRsaSignatureSize);

    const auto fingerprint = certFingerprintHex(id.certPem);
    REQUIRE(fingerprint.has_value());
    CHECK(fingerprint->size() == 64);
}

TEST_CASE("RSA-SHA256 sign/verify round-trips and rejects tampering", "[moonlight][crypto]") {
    const auto& id = testIdentity();
    std::vector<std::uint8_t> secret(kPairingSecretSize, 0x5A);

    const auto signature = rsaSignSha256(id.privateKeyPem, secret.data(), secret.size());
    REQUIRE(signature.has_value());
    CHECK(signature->size() == kRsaSignatureSize);

    CHECK(rsaVerifySha256(id.certPem, secret.data(), secret.size(), signature->data(),
                          signature->size()));

    SECTION("tampered message fails") {
        auto altered = secret;
        altered[0] ^= 0x01;
        CHECK_FALSE(rsaVerifySha256(id.certPem, altered.data(), altered.size(), signature->data(),
                                    signature->size()));
    }
    SECTION("tampered signature fails") {
        auto badSig = *signature;
        badSig[10] ^= 0x01;
        CHECK_FALSE(rsaVerifySha256(id.certPem, secret.data(), secret.size(), badSig.data(),
                                    badSig.size()));
    }
    SECTION("a different identity's cert fails") {
        const auto other = generateClientIdentity();
        REQUIRE(other.has_value());
        CHECK_FALSE(rsaVerifySha256(other->certPem, secret.data(), secret.size(), signature->data(),
                                    signature->size()));
    }
}

TEST_CASE("cert helpers reject garbage input", "[moonlight][crypto]") {
    CHECK_FALSE(isValidCertPem("not a pem"));
    CHECK_FALSE(certSignature("not a pem").has_value());
    CHECK_FALSE(certFingerprintHex("").has_value());
}
