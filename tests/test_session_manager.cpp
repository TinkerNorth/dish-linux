// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// WifiConnectionManager drives REST through a concrete HTTPClient with no
// injectable gateway seam, so its async FSM can't be unit-driven without
// sockets. These re-derive the decision rules it composes as pure logic.

#include "core/net/IpLiterals.h"
#include "core/reducer/Backoff.h"
#include "core/reducer/CloseNotify.h"
#include "core/reducer/RestOutcome.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace reducer = dish::reducer;

namespace {

enum class Intent { UserInitiated, AutoReconnect, RetryAfterDeath };

// Only a user tap is loud (the manager's emitErrorIfUserInitiated).
bool isLoud(Intent i) { return i == Intent::UserInitiated; }

// Every intent except a user tap rides the backoff, since scheduleRetry returns
// early for UserInitiated.
bool schedulesRetry(Intent i) { return i != Intent::UserInitiated; }

} // namespace

TEST_CASE("manager intent: only UserInitiated is loud on failure", "[manager][intent]") {
    REQUIRE(isLoud(Intent::UserInitiated));
    REQUIRE_FALSE(isLoud(Intent::AutoReconnect));
    REQUIRE_FALSE(isLoud(Intent::RetryAfterDeath));
}

TEST_CASE("manager intent: silent intents schedule a backoff retry; a user tap does not",
          "[manager][intent]") {
    REQUIRE_FALSE(schedulesRetry(Intent::UserInitiated));
    REQUIRE(schedulesRetry(Intent::AutoReconnect));
    REQUIRE(schedulesRetry(Intent::RetryAfterDeath));
}

// A terminal verdict stops the curve under every intent; only the toast is gated.

TEST_CASE("manager: a terminal verdict never rides the silent retry curve", "[manager][terminal]") {
    REQUIRE(reducer::restVerdictTerminal(reducer::RestVerdict::Unauthorized));
    REQUIRE(reducer::restVerdictTerminal(reducer::RestVerdict::VersionMismatch));
    REQUIRE_FALSE(reducer::restVerdictRetryable(reducer::RestVerdict::Unauthorized));
    REQUIRE_FALSE(reducer::restVerdictRetryable(reducer::RestVerdict::VersionMismatch));
}

TEST_CASE("manager: an unreachable/503/5xx verdict is retryable (silent backoff territory)",
          "[manager][terminal]") {
    REQUIRE(reducer::restVerdictRetryable(reducer::RestVerdict::Unreachable));
    REQUIRE(reducer::restVerdictRetryable(reducer::RestVerdict::ShuttingDown));
    REQUIRE(reducer::restVerdictRetryable(reducer::RestVerdict::ServerError));
    REQUIRE_FALSE(reducer::restVerdictTerminal(reducer::RestVerdict::Unreachable));
}

// The manager's pairAndConnect branches on these arms: Success -> store key +
// openSession; AuthRequired/Pending -> PIN or Stale; VersionMismatch -> explain;
// Unreachable -> toast or Stale, by intent.

namespace {
reducer::PairReply pairReply(int status, bool parsed, bool ok, bool pending, bool hasKey) {
    reducer::PairReply r;
    r.status = status;
    r.bodyParsed = parsed;
    r.ok = ok;
    r.pending = pending;
    r.hasSharedKey = hasKey;
    return r;
}
} // namespace

TEST_CASE("manager pair: an ok reply with a key is Success (open the session)", "[manager][pair]") {
    REQUIRE(reducer::classifyPair(pairReply(200, true, true, false, true)) ==
            reducer::PairVerdict::Success);
}

TEST_CASE("manager pair: empty/unparsed (status 0) is Unreachable, not PairingRequired",
          "[manager][pair]") {
    REQUIRE(reducer::classifyPair(pairReply(0, false, false, false, false)) ==
            reducer::PairVerdict::Unreachable);
}

TEST_CASE("manager pair: ok=false reachable is AuthRequired (PIN territory)", "[manager][pair]") {
    REQUIRE(reducer::classifyPair(pairReply(200, true, false, false, false)) ==
            reducer::PairVerdict::AuthRequired);
}

TEST_CASE("manager pair: a 409 is a protocol mismatch, not the PIN dialog", "[manager][pair]") {
    REQUIRE(reducer::classifyPair(pairReply(409, true, false, false, false)) ==
            reducer::PairVerdict::VersionMismatch);
}

TEST_CASE("manager pair: ok=true with no key is AuthRequired (never cache an empty key)",
          "[manager][pair]") {
    REQUIRE(reducer::classifyPair(pairReply(200, true, true, false, false)) ==
            reducer::PairVerdict::AuthRequired);
}

TEST_CASE("manager close: unpaired drops the key and stops retrying", "[manager][close]") {
    REQUIRE(reducer::closeActionForReason(dish::proto::kCloseReasonUnpaired) ==
            reducer::CloseAction::DropKeyRePair);
}

TEST_CASE("manager close: replaced stays down (a newer PUT owns the session)", "[manager][close]") {
    REQUIRE(reducer::closeActionForReason(dish::proto::kCloseReasonReplaced) ==
            reducer::CloseAction::StayDown);
}

TEST_CASE("manager close: shutdown and kicked reconnect on the backoff curve", "[manager][close]") {
    REQUIRE(reducer::closeActionForReason(dish::proto::kCloseReasonShutdown) ==
            reducer::CloseAction::RetryBackoff);
    REQUIRE(reducer::closeActionForReason(dish::proto::kCloseReasonKicked) ==
            reducer::CloseAction::RetryBackoff);
}

TEST_CASE("manager guard: a public ip target is refused (private-literal rule)",
          "[manager][guard]") {
    // connectTo consults this before any socket work; a satellite only ever
    // legitimately lives on the LAN.
    REQUIRE_FALSE(dish::net::isPrivateHostLiteral("8.8.8.8"));
    REQUIRE(dish::net::isPrivateHostLiteral("10.0.0.5"));
    REQUIRE(dish::net::isPrivateHostLiteral("192.168.1.20"));
    REQUIRE(dish::net::isPrivateHostLiteral("172.16.0.9"));
}

TEST_CASE("manager backoff: first silent retry is 1s, the curve is bounded at 60s",
          "[manager][backoff]") {
    REQUIRE(reducer::backoffDelayMs(1) == 1000);
    REQUIRE(reducer::backoffDelayMs(100) == reducer::kBackoffMaxMs);
}
