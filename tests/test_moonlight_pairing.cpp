// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Round-trips the whole PIN handshake in both directions: the client side is
// the production PairingSession, the host side is re-derived here from the
// same primitives, following the protocol's host algorithm (Wolf's
// moonlight.cpp pair functions). Every random input is fixed, so failures
// reproduce byte-for-byte.

#include "core/moonlight/MoonlightPairing.h"
#include "core/moonlight/MoonlightPairingCrypto.h"

#include "Util/Hex.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace dish;
using namespace dish::mooncrypto;
using dish::moonpair::PairingSession;

namespace {

std::array<std::uint8_t, 16> patternBytes(std::uint8_t seed) {
    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i * 7);
    }
    return out;
}

std::string upperHex(const Bytes& bytes) {
    std::string hex = util::toHex(bytes);
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return hex;
}

Bytes mustHexDecode(const std::string& hex) {
    const auto decoded = util::fromHex(hex);
    REQUIRE(decoded.has_value());
    return *decoded;
}

// The host side of the handshake, mirroring the protocol's server algorithm
// with deterministic secrets.
struct FakeHost {
    explicit FakeHost(ClientIdentity id) : identity(std::move(id)) {}

    ClientIdentity identity;
    std::array<std::uint8_t, 16> aesKey{};
    std::array<std::uint8_t, 16> serverSecret = patternBytes(0x21);
    std::array<std::uint8_t, 16> serverChallenge = patternBytes(0x87);

    Bytes clientChallenge; // decrypted in phase 2
    Bytes clientHash;      // decrypted in phase 3

    // Phase 1: derive the key, hand back the cert.
    std::string phase1(const std::string& saltHex, const std::string& pinTyped) {
        const auto saltBytes = mustHexDecode(saltHex);
        REQUIRE(saltBytes.size() == 16);
        std::array<std::uint8_t, 16> salt{};
        std::copy(saltBytes.begin(), saltBytes.end(), salt.begin());
        aesKey = derivePairingKey(salt, pinTyped);
        return upperHex(Bytes(identity.certPem.begin(), identity.certPem.end()));
    }

    // Phase 2: decrypt the challenge, answer hash(challenge + own cert
    // signature + server secret) + server challenge, encrypted.
    std::string phase2(const std::string& clientChallengeHex) {
        const auto encrypted = mustHexDecode(clientChallengeHex);
        const auto decrypted = aesEcbDecrypt(aesKey, encrypted.data(), encrypted.size());
        REQUIRE(decrypted.has_value());
        clientChallenge = *decrypted;

        const auto certSig = certSignature(identity.certPem);
        REQUIRE(certSig.has_value());
        Bytes material = clientChallenge;
        material.insert(material.end(), certSig->begin(), certSig->end());
        material.insert(material.end(), serverSecret.begin(), serverSecret.end());
        const auto hash = sha256(material.data(), material.size());

        Bytes response(hash.begin(), hash.end());
        response.insert(response.end(), serverChallenge.begin(), serverChallenge.end());
        const auto sealed = aesEcbEncrypt(aesKey, response.data(), response.size());
        REQUIRE(sealed.has_value());
        return upperHex(*sealed);
    }

    // Phase 3: decrypt the client hash, answer secret + signature.
    std::string phase3(const std::string& serverChallengeRespHex) {
        const auto encrypted = mustHexDecode(serverChallengeRespHex);
        const auto decrypted = aesEcbDecrypt(aesKey, encrypted.data(), encrypted.size());
        REQUIRE(decrypted.has_value());
        clientHash = *decrypted;

        const auto signature =
            rsaSignSha256(identity.privateKeyPem, serverSecret.data(), serverSecret.size());
        REQUIRE(signature.has_value());
        Bytes payload(serverSecret.begin(), serverSecret.end());
        payload.insert(payload.end(), signature->begin(), signature->end());
        return upperHex(payload);
    }

    // Phase 4: the host's verdict over the client's secret + signature.
    bool phase4(const std::string& clientPairingSecretHex, const std::string& clientCertPem) {
        const auto payload = mustHexDecode(clientPairingSecretHex);
        if (payload.size() < 16 + kRsaSignatureSize) { return false; }
        const std::uint8_t* clientSecret = payload.data();
        const std::uint8_t* signature = payload.data() + 16;

        const auto clientCertSig = certSignature(clientCertPem);
        if (!clientCertSig) { return false; }
        Bytes material(serverChallenge.begin(), serverChallenge.end());
        material.insert(material.end(), clientCertSig->begin(), clientCertSig->end());
        material.insert(material.end(), clientSecret, clientSecret + 16);
        const auto expected = sha256(material.data(), material.size());
        if (clientHash.size() != expected.size() ||
            std::memcmp(clientHash.data(), expected.data(), expected.size()) != 0) {
            return false;
        }
        return rsaVerifySha256(clientCertPem, clientSecret, 16, signature, kRsaSignatureSize);
    }
};

const ClientIdentity& clientIdentity() {
    static const ClientIdentity id = [] {
        const auto generated = generateClientIdentity();
        REQUIRE(generated.has_value());
        return *generated;
    }();
    return id;
}

const ClientIdentity& hostIdentity() {
    static const ClientIdentity id = [] {
        const auto generated = generateClientIdentity();
        REQUIRE(generated.has_value());
        return *generated;
    }();
    return id;
}

PairingSession makeSession(const std::string& pin) {
    return PairingSession(clientIdentity().certPem, clientIdentity().privateKeyPem,
                          patternBytes(0x01), pin, patternBytes(0x43), patternBytes(0x65));
}

} // namespace

TEST_CASE("pinFromRandom keeps leading zeros", "[moonlight][pairing]") {
    CHECK(dish::moonpair::pinFromRandom(0) == "0000");
    CHECK(dish::moonpair::pinFromRandom(42) == "0042");
    CHECK(dish::moonpair::pinFromRandom(19999) == "9999");
    CHECK(dish::moonpair::pinFromRandom(1234) == "1234");
}

TEST_CASE("full handshake succeeds on both ends with the right PIN", "[moonlight][pairing]") {
    FakeHost host(hostIdentity());
    PairingSession session = makeSession("4989");

    // Phase 1.
    const std::string plaincert = host.phase1(session.saltHex(), "4989");
    REQUIRE(session.acceptServerCert(plaincert));
    CHECK(session.serverCertPem() == hostIdentity().certPem);

    // Phase 2.
    const auto challenge = session.clientChallengeHex();
    REQUIRE(challenge.has_value());
    const std::string challengeResponse = host.phase2(*challenge);
    const auto serverChallengeResp = session.acceptChallengeResponse(challengeResponse);
    REQUIRE(serverChallengeResp.has_value());

    // The host decrypted our real challenge bytes.
    CHECK(util::toHex(host.clientChallenge) == util::toHex(patternBytes(0x43).data(), 16));

    // Phase 3: the client accepts the host's secret + signature.
    const std::string pairingSecret = host.phase3(*serverChallengeResp);
    CHECK(session.acceptPairingSecret(pairingSecret));

    // Phase 4: the host accepts the client's secret + signature.
    const auto clientPairingSecret = session.clientPairingSecretHex();
    REQUIRE(clientPairingSecret.has_value());
    CHECK(host.phase4(*clientPairingSecret, clientIdentity().certPem));
}

TEST_CASE("a wrong PIN is detected by the client at phase 3", "[moonlight][pairing]") {
    FakeHost host(hostIdentity());
    PairingSession session = makeSession("4989"); // client's PIN differs

    host.phase1(session.saltHex(), "1111");
    REQUIRE(session.acceptServerCert(
        upperHex(Bytes(hostIdentity().certPem.begin(), hostIdentity().certPem.end()))));
    const auto challenge = session.clientChallengeHex();
    REQUIRE(challenge.has_value());
    const std::string challengeResponse = host.phase2(*challenge);
    const auto serverChallengeResp = session.acceptChallengeResponse(challengeResponse);
    // The decrypt "succeeds" but yields noise on both sides…
    REQUIRE(serverChallengeResp.has_value());
    // …so the hash check MUST fail when the host reveals its secret.
    CHECK_FALSE(session.acceptPairingSecret(host.phase3(*serverChallengeResp)));
}

TEST_CASE("a substituted server certificate is rejected", "[moonlight][pairing]") {
    FakeHost host(hostIdentity());
    PairingSession session = makeSession("4989");

    host.phase1(session.saltHex(), "4989");
    // A man in the middle presents its own cert but cannot sign with the real
    // host key behind the phase-2 hash material.
    REQUIRE(session.acceptServerCert(
        upperHex(Bytes(clientIdentity().certPem.begin(), clientIdentity().certPem.end()))));
    const auto challenge = session.clientChallengeHex();
    REQUIRE(challenge.has_value());
    const auto serverChallengeResp = session.acceptChallengeResponse(host.phase2(*challenge));
    REQUIRE(serverChallengeResp.has_value());
    CHECK_FALSE(session.acceptPairingSecret(host.phase3(*serverChallengeResp)));
}

TEST_CASE("a tampered pairing secret signature is rejected", "[moonlight][pairing]") {
    FakeHost host(hostIdentity());
    PairingSession session = makeSession("4989");

    host.phase1(session.saltHex(), "4989");
    REQUIRE(session.acceptServerCert(
        upperHex(Bytes(hostIdentity().certPem.begin(), hostIdentity().certPem.end()))));
    const auto challenge = session.clientChallengeHex();
    REQUIRE(challenge.has_value());
    const auto serverChallengeResp = session.acceptChallengeResponse(host.phase2(*challenge));
    REQUIRE(serverChallengeResp.has_value());

    std::string pairingSecret = host.phase3(*serverChallengeResp);
    // Flip a nibble inside the signature half.
    const std::size_t at = pairingSecret.size() - 3;
    pairingSecret[at] = pairingSecret[at] == '0' ? '1' : '0';
    CHECK_FALSE(session.acceptPairingSecret(pairingSecret));
}

TEST_CASE("malformed responses are refused", "[moonlight][pairing]") {
    PairingSession session = makeSession("4989");

    SECTION("plaincert that is not hex") { CHECK_FALSE(session.acceptServerCert("zz-not-hex")); }
    SECTION("plaincert that is hex but not a certificate") {
        CHECK_FALSE(session.acceptServerCert("41414141"));
    }
    SECTION("clientChallengeHex needs the server cert first") {
        CHECK_FALSE(session.clientChallengeHex().has_value());
    }
    SECTION("short challengeresponse") {
        REQUIRE(session.acceptServerCert(
            upperHex(Bytes(hostIdentity().certPem.begin(), hostIdentity().certPem.end()))));
        CHECK_FALSE(session.acceptChallengeResponse("00112233").has_value());
    }
    SECTION("pairing secret before the challenge phases") {
        CHECK_FALSE(session.acceptPairingSecret(std::string(544, 'A')));
    }
}
