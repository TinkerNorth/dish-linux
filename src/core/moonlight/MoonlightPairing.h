// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The client side of the 5-phase Moonlight PIN pairing handshake, as pure
// computation: every random input is injected, so the whole exchange is
// deterministic and unit-testable in both directions. The HTTP transport lives
// in source/moonlight; this class only turns responses into the next request's
// parameters and verdicts.
//
// Protocol (Wolf docs/http-pairing.adoc + moonlight.cpp, mirrored client-side):
//   1. -> salt + client cert PEM (hex);            <- server cert PEM (hex).
//      Both ends derive AES = SHA-256(salt||PIN)[0:16].
//   2. -> AES-ECB(client challenge) (hex);         <- AES-ECB(server response
//      hash(32) + server challenge(16)).
//   3. -> AES-ECB(SHA-256(server challenge + client cert signature + client
//      secret)) (hex);                             <- server secret(16) +
//      RSA-SHA256 signature(256) (hex). The client now checks the phase-2
//      hash == SHA-256(client challenge + server cert signature + server
//      secret) AND that the secret's signature verifies against the server
//      cert.
//   4. -> client secret(16) + RSA-SHA256 signature(256) (hex); <- paired=1.
//   5. over HTTPS with the client cert: phrase=pairchallenge;  <- paired=1.

#pragma once

#include "core/moonlight/MoonlightPairingCrypto.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dish::moonpair {

// The PIN the user types into the host, 4 digits with leading zeros kept.
std::string pinFromRandom(std::uint32_t random);

class PairingSession {
  public:
    // `salt`, `clientChallenge` and `clientSecret` are the handshake's three
    // random 16-byte inputs; production callers fill them from the CSPRNG,
    // tests pass fixed bytes.
    PairingSession(std::string clientCertPem, std::string clientKeyPem,
                   const std::array<std::uint8_t, 16>& salt, const std::string& pin,
                   const std::array<std::uint8_t, 16>& clientChallenge,
                   const std::array<std::uint8_t, 16>& clientSecret);

    // Phase 1 query parameters (uppercase hex, the wire convention).
    std::string saltHex() const;
    std::string clientCertHex() const;

    // Phase 1 response: the server's `plaincert` (hex-encoded PEM). False on
    // undecodable hex or a string that is not a certificate.
    bool acceptServerCert(const std::string& plaincertHex);
    const std::string& serverCertPem() const { return serverCertPem_; }

    // Phase 2 request: `clientchallenge`. nullopt before acceptServerCert or on
    // a crypto failure.
    std::optional<std::string> clientChallengeHex() const;

    // Phase 2 response -> phase 3 request: decrypts `challengeresponse`, holds
    // the server's response hash for the phase-3 check, and returns
    // `serverchallengeresp`. nullopt on malformed input.
    std::optional<std::string> acceptChallengeResponse(const std::string& challengeResponseHex);

    // Phase 3 response: `pairingsecret`. True only when the server proves
    // knowledge of the PIN-derived key (hash check) AND of its certificate's
    // private key (signature check). False otherwise — treat as wrong PIN or a
    // man in the middle and abort.
    bool acceptPairingSecret(const std::string& pairingSecretHex);

    // Phase 4 request: `clientpairingsecret`. nullopt on a signing failure.
    std::optional<std::string> clientPairingSecretHex() const;

  private:
    std::string clientCertPem_;
    std::string clientKeyPem_;
    std::array<std::uint8_t, 16> salt_{};
    std::array<std::uint8_t, mooncrypto::kAesKeySize> aesKey_{};
    std::array<std::uint8_t, 16> clientChallenge_{};
    std::array<std::uint8_t, 16> clientSecret_{};

    std::string serverCertPem_;
    std::array<std::uint8_t, mooncrypto::kSha256Size> serverResponseHash_{};
    std::array<std::uint8_t, 16> serverChallenge_{};
    bool haveChallengeResponse_ = false;
};

} // namespace dish::moonpair
