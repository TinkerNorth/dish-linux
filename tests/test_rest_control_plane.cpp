// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/RestOutcome.h"
#include "core/wire/SessionCrypto.h"
#include "Network/PairingClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <variant>

namespace reducer = dish::reducer;
namespace wire = dish::wire;

namespace {
reducer::RestReply reply(int status, bool parsed, const char* code = "", bool pinMismatch = false) {
    reducer::RestReply r;
    r.status = status;
    r.bodyParsed = parsed;
    r.code = code;
    r.pinMismatch = pinMismatch;
    return r;
}

reducer::PairReply pairReply(int status, bool parsed, bool ok = false, bool pending = false,
                             bool hasKey = false, bool pinMismatch = false) {
    reducer::PairReply r;
    r.status = status;
    r.bodyParsed = parsed;
    r.ok = ok;
    r.pending = pending;
    r.hasSharedKey = hasKey;
    r.pinMismatch = pinMismatch;
    return r;
}
} // namespace

TEST_CASE("classifyRest: 2xx with a parsed body is Ok", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(200, true)) == reducer::RestVerdict::Ok);
    REQUIRE(reducer::classifyRest(reply(204, true)) == reducer::RestVerdict::Ok);
}

TEST_CASE("classifyRest: 401 NOT_PAIRED / BAD_PROOF is terminal Unauthorized", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(401, true, "NOT_PAIRED")) ==
            reducer::RestVerdict::Unauthorized);
    REQUIRE(reducer::classifyRest(reply(401, true, "BAD_PROOF")) ==
            reducer::RestVerdict::Unauthorized);
    REQUIRE(reducer::restVerdictTerminal(reducer::RestVerdict::Unauthorized));
    REQUIRE_FALSE(reducer::restVerdictRetryable(reducer::RestVerdict::Unauthorized));
}

TEST_CASE("classifyRest: 409 is terminal VersionMismatch", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(409, true)) == reducer::RestVerdict::VersionMismatch);
    REQUIRE(reducer::restVerdictTerminal(reducer::RestVerdict::VersionMismatch));
}

TEST_CASE("classifyRest: 503 is retryable ShuttingDown", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(503, true)) == reducer::RestVerdict::ShuttingDown);
    REQUIRE(reducer::restVerdictRetryable(reducer::RestVerdict::ShuttingDown));
    REQUIRE_FALSE(reducer::restVerdictTerminal(reducer::RestVerdict::ShuttingDown));
}

TEST_CASE("classifyRest: status 0 / unparsed body is Unreachable", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(0, false)) == reducer::RestVerdict::Unreachable);
    REQUIRE(reducer::classifyRest(reply(200, false)) == reducer::RestVerdict::Unreachable);
    REQUIRE(reducer::restVerdictRetryable(reducer::RestVerdict::Unreachable));
}

TEST_CASE("classifyRest: other 5xx with a body is a retryable ServerError", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(500, true)) == reducer::RestVerdict::ServerError);
    REQUIRE(reducer::restVerdictRetryable(reducer::RestVerdict::ServerError));
}

// ── TOFU identity change ────────────────────────────────────────────────────
// A pin mismatch aborts the handshake, so the reply that reaches the classifier
// is statusless and bodiless — byte-identical to a dead link. Only the flag
// separates them, and getting that wrong is what made a reinstalled satellite
// (or a MITM) look "unreachable" and earn an unbounded retry curve.

TEST_CASE("classifyRest: a pin mismatch is a terminal IdentityChanged", "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(0, false, "", /*pinMismatch=*/true)) ==
            reducer::RestVerdict::IdentityChanged);
    REQUIRE(reducer::restVerdictTerminal(reducer::RestVerdict::IdentityChanged));
    REQUIRE_FALSE(reducer::restVerdictRetryable(reducer::RestVerdict::IdentityChanged));
}

TEST_CASE("classifyRest: a pin mismatch outranks the whole status ladder", "[rest][classify]") {
    // Nothing below it is trustworthy: the abort means no proof ever transited.
    for (const int status : {0, 200, 401, 409, 500, 503}) {
        REQUIRE(reducer::classifyRest(reply(status, true, "NOT_PAIRED", /*pinMismatch=*/true)) ==
                reducer::RestVerdict::IdentityChanged);
    }
}

TEST_CASE("classifyRest: without a mismatch every existing verdict is unchanged",
          "[rest][classify]") {
    REQUIRE(reducer::classifyRest(reply(200, true)) == reducer::RestVerdict::Ok);
    REQUIRE(reducer::classifyRest(reply(401, true, "NOT_PAIRED")) ==
            reducer::RestVerdict::Unauthorized);
    REQUIRE(reducer::classifyRest(reply(409, true)) == reducer::RestVerdict::VersionMismatch);
    REQUIRE(reducer::classifyRest(reply(503, true)) == reducer::RestVerdict::ShuttingDown);
    REQUIRE(reducer::classifyRest(reply(500, true)) == reducer::RestVerdict::ServerError);
    REQUIRE(reducer::classifyRest(reply(0, false)) == reducer::RestVerdict::Unreachable);
}

TEST_CASE("classifyPair: a pin mismatch is a terminal IdentityChanged", "[rest][classify]") {
    REQUIRE(reducer::classifyPair(pairReply(0, false, false, false, false,
                                            /*pinMismatch=*/true)) ==
            reducer::PairVerdict::IdentityChanged);
    // Ahead of 409, which is the only other arm keyed off the status alone.
    REQUIRE(reducer::classifyPair(pairReply(409, true, false, false, false,
                                            /*pinMismatch=*/true)) ==
            reducer::PairVerdict::IdentityChanged);
}

TEST_CASE("classifyPair: without a mismatch every existing verdict is unchanged",
          "[rest][classify]") {
    REQUIRE(reducer::classifyPair(pairReply(200, true, true, false, true)) ==
            reducer::PairVerdict::Success);
    REQUIRE(reducer::classifyPair(pairReply(200, true, false, true)) ==
            reducer::PairVerdict::Pending);
    REQUIRE(reducer::classifyPair(pairReply(200, true)) == reducer::PairVerdict::AuthRequired);
    REQUIRE(reducer::classifyPair(pairReply(409, true)) == reducer::PairVerdict::VersionMismatch);
    REQUIRE(reducer::classifyPair(pairReply(0, false)) == reducer::PairVerdict::Unreachable);
}

// PairingClient::classify is the arm the manager actually visits; it lives here
// beside the verdict it wraps rather than in the PIN-path suite.
TEST_CASE("PairingClient::classify: a pin mismatch yields the IdentityChanged arm",
          "[rest][classify]") {
    using dish::net::PairingClient;
    dish::models::PairResponse r;
    r.httpStatus = 0;
    r.reachable = false;
    REQUIRE(std::holds_alternative<PairingClient::IdentityChanged>(
        PairingClient::classify(r, /*pinMismatch=*/true)));
    // The default keeps the pre-existing single-argument reading of the reply.
    REQUIRE(std::holds_alternative<PairingClient::Unreachable>(PairingClient::classify(r)));
}

TEST_CASE("classifyApproval: approved with a key", "[rest][approval]") {
    reducer::ApprovalReply r;
    r.status = 200;
    r.bodyParsed = true;
    r.statusStr = "approved";
    r.hasSharedKey = true;
    REQUIRE(reducer::classifyApproval(r) == reducer::ApprovalVerdict::Approved);
}

TEST_CASE("classifyApproval: approved without a key is still Pending", "[rest][approval]") {
    // The staged key is single-use; an "approved" with no key (already consumed)
    // must not be mistaken for a fresh grant.
    reducer::ApprovalReply r;
    r.status = 200;
    r.bodyParsed = true;
    r.statusStr = "approved";
    r.hasSharedKey = false;
    REQUIRE(reducer::classifyApproval(r) == reducer::ApprovalVerdict::Pending);
}

TEST_CASE("classifyApproval: denied / pending / none / unreachable", "[rest][approval]") {
    auto verdict = [](const char* status, int httpStatus, bool parsed) {
        reducer::ApprovalReply r;
        r.status = httpStatus;
        r.bodyParsed = parsed;
        r.statusStr = status;
        return reducer::classifyApproval(r);
    };
    REQUIRE(verdict("denied", 200, true) == reducer::ApprovalVerdict::Declined);
    REQUIRE(verdict("pending", 200, true) == reducer::ApprovalVerdict::Pending);
    REQUIRE(verdict("none", 200, true) == reducer::ApprovalVerdict::Pending);
    REQUIRE(verdict("pending", 0, false) == reducer::ApprovalVerdict::Unreachable);
}

TEST_CASE("hmacProof header value matches the pinned interop vector", "[rest][auth]") {
    // X-Hmac-Proof = computeHmacProof(pairingKey, deviceId), pinned against the
    // vector the other implementations assert: pairingKey = 01 02 .. 20,
    // deviceId = "device-1".
    std::array<std::uint8_t, wire::kCryptoKeySize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) { key[i] = static_cast<std::uint8_t>(i + 1); }
    REQUIRE(wire::computeHmacProof(key.data(), "device-1") ==
            "05a035a10c55fdfe254c9df5df55a614ac128b123a5de225ea33b41f1d4eedde");
}

TEST_CASE("the client-to-server send framing decrypts as the satellite expects", "[rest][wire]") {
    // One packet the way SatelliteClient builds it: the FIRST send uses counter
    // 1 (not 0), nonce direction CLIENT_TO_SERVER, AAD = token (4 BE).
    std::array<std::uint8_t, wire::kCryptoKeySize> sessionKey{};
    sessionKey[0] = 0xAB; // arbitrary derived key
    const std::uint32_t token = 0x0007a1b2u;

    // Heartbeat inner frame: type(2 BE) | len(2 BE) | empty payload.
    const std::uint8_t inner[4] = {0x00, 0x02, 0x00, 0x00};

    std::uint8_t ct[64];
    unsigned long long ctLen = 0;
    REQUIRE(wire::encryptPacket(sessionKey.data(), wire::kDirClientToServer, /*counter=*/1, token,
                                inner, sizeof(inner), ct, &ctLen));

    std::uint8_t pt[64];
    unsigned long long ptLen = 0;
    REQUIRE(wire::decryptPacket(sessionKey.data(), wire::kDirClientToServer, 1, token, ct,
                                static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE(ptLen == sizeof(inner));
    REQUIRE(std::memcmp(pt, inner, sizeof(inner)) == 0);

    // An off-by-one counter and the wrong direction byte both fail auth.
    REQUIRE_FALSE(wire::decryptPacket(sessionKey.data(), wire::kDirClientToServer, 0, token, ct,
                                      static_cast<std::size_t>(ctLen), pt, &ptLen));
    REQUIRE_FALSE(wire::decryptPacket(sessionKey.data(), wire::kDirServerToClient, 1, token, ct,
                                      static_cast<std::size_t>(ctLen), pt, &ptLen));
}

// There is no wire "denied": an operator deny erases the pending row, so the
// client polls straight to "none". Terminal only once a "pending" was seen,
// otherwise "none" would lose the POST-to-first-poll race.

TEST_CASE("classifyApproval: none before any pending keeps waiting", "[rest][approval]") {
    dish::reducer::ApprovalReply r;
    r.status = 200;
    r.bodyParsed = true;
    r.statusStr = "none";
    REQUIRE(dish::reducer::classifyApproval(r, /*sawPending=*/false) ==
            dish::reducer::ApprovalVerdict::Pending);
}

TEST_CASE("classifyApproval: none AFTER a pending is a terminal decline", "[rest][approval]") {
    dish::reducer::ApprovalReply r;
    r.status = 200;
    r.bodyParsed = true;
    r.statusStr = "none";
    REQUIRE(dish::reducer::classifyApproval(r, /*sawPending=*/true) ==
            dish::reducer::ApprovalVerdict::Declined);
}

TEST_CASE("classifyApproval: legacy denied still declines regardless of pending",
          "[rest][approval]") {
    dish::reducer::ApprovalReply r;
    r.status = 200;
    r.bodyParsed = true;
    r.statusStr = "denied";
    REQUIRE(dish::reducer::classifyApproval(r, false) == dish::reducer::ApprovalVerdict::Declined);
    REQUIRE(dish::reducer::classifyApproval(r, true) == dish::reducer::ApprovalVerdict::Declined);
}

TEST_CASE("classifyApproval: pending stays pending with the flag either way", "[rest][approval]") {
    dish::reducer::ApprovalReply r;
    r.status = 200;
    r.bodyParsed = true;
    r.statusStr = "pending";
    REQUIRE(dish::reducer::classifyApproval(r, false) == dish::reducer::ApprovalVerdict::Pending);
    REQUIRE(dish::reducer::classifyApproval(r, true) == dish::reducer::ApprovalVerdict::Pending);
}
