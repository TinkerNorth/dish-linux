// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Socket-free and clock-free by construction: the live WifiConnectionManager
// poll methods open real Winsock work the test process would have to join, so
// the loop's branching lives in reducer::nextReversePairingAction instead.

#include "core/reducer/ReversePairing.h"
#include "core/reducer/RestOutcome.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace reducer = dish::reducer;

using reducer::ApprovalVerdict;
using reducer::nextReversePairingAction;
using reducer::ReversePairingAction;

namespace {
// The exact budget is irrelevant to the rule.
constexpr std::int64_t kDeadline = 120'000;
} // namespace

TEST_CASE("nextReversePairingAction: Approved -> Approve regardless of elapsed",
          "[reverse][action]") {
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Approved, 0, kDeadline) ==
            ReversePairingAction::Approve);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Approved, kDeadline + 5000, kDeadline) ==
            ReversePairingAction::Approve);
}

TEST_CASE("nextReversePairingAction: Declined -> Decline regardless of elapsed",
          "[reverse][action]") {
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Declined, 0, kDeadline) ==
            ReversePairingAction::Decline);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Declined, kDeadline + 5000, kDeadline) ==
            ReversePairingAction::Decline);
}

TEST_CASE("nextReversePairingAction: Pending under the deadline keeps polling",
          "[reverse][action]") {
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Pending, 0, kDeadline) ==
            ReversePairingAction::KeepPolling);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Pending, kDeadline - 1, kDeadline) ==
            ReversePairingAction::KeepPolling);
}

TEST_CASE("nextReversePairingAction: Unreachable under the deadline keeps polling",
          "[reverse][action]") {
    // A momentary transport blip during the wait must NOT abort the pair.
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Unreachable, 0, kDeadline) ==
            ReversePairingAction::KeepPolling);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Unreachable, kDeadline - 1, kDeadline) ==
            ReversePairingAction::KeepPolling);
}

TEST_CASE("nextReversePairingAction: elapsed == deadline times out (inclusive boundary)",
          "[reverse][action]") {
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Pending, kDeadline, kDeadline) ==
            ReversePairingAction::TimeOut);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Unreachable, kDeadline, kDeadline) ==
            ReversePairingAction::TimeOut);
}

TEST_CASE("nextReversePairingAction: elapsed past the deadline times out", "[reverse][action]") {
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Pending, kDeadline + 1, kDeadline) ==
            ReversePairingAction::TimeOut);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Unreachable, kDeadline + 1000, kDeadline) ==
            ReversePairingAction::TimeOut);
}

TEST_CASE("nextReversePairingAction: a non-positive deadline times out on the first wait",
          "[reverse][action]") {
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Pending, 0, 0) ==
            ReversePairingAction::TimeOut);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Unreachable, 0, -1) ==
            ReversePairingAction::TimeOut);
    // A terminal verdict still resolves with no budget.
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Approved, 0, 0) ==
            ReversePairingAction::Approve);
    REQUIRE(nextReversePairingAction(ApprovalVerdict::Declined, 0, 0) ==
            ReversePairingAction::Decline);
}

TEST_CASE("the status-reply verdict drives the poll action end-to-end", "[reverse][action]") {
    auto action = [](const char* statusStr, bool hasKey, std::int64_t elapsed) {
        reducer::ApprovalReply ar;
        ar.status = 200;
        ar.bodyParsed = true;
        ar.statusStr = statusStr;
        ar.hasSharedKey = hasKey;
        return nextReversePairingAction(reducer::classifyApproval(ar), elapsed, kDeadline);
    };
    REQUIRE(action("approved", true, 5000) == ReversePairingAction::Approve);
    // "approved" with no key degrades to Pending upstream.
    REQUIRE(action("approved", false, 5000) == ReversePairingAction::KeepPolling);
    REQUIRE(action("denied", false, 5000) == ReversePairingAction::Decline);
    REQUIRE(action("pending", false, 5000) == ReversePairingAction::KeepPolling);
    REQUIRE(action("none", false, 5000) == ReversePairingAction::KeepPolling);
    REQUIRE(action("pending", false, kDeadline) == ReversePairingAction::TimeOut);
}

TEST_CASE("formatReversePin always yields exactly 4 decimal digits", "[reverse][pin]") {
    REQUIRE(reducer::formatReversePin(0) == "0000");
    REQUIRE(reducer::formatReversePin(7) == "0007");
    REQUIRE(reducer::formatReversePin(42) == "0042");
    REQUIRE(reducer::formatReversePin(1234) == "1234");
    REQUIRE(reducer::formatReversePin(9999) == "9999");
}

TEST_CASE("formatReversePin reduces an out-of-range draw modulo 10000", "[reverse][pin]") {
    REQUIRE(reducer::formatReversePin(10000) == "0000");
    REQUIRE(reducer::formatReversePin(10042) == "0042");
    REQUIRE(reducer::formatReversePin(123456) == "3456");
    REQUIRE(reducer::formatReversePin(0xFFFFFFFFu) == "7295"); // 4294967295 % 10000
}

TEST_CASE("formatReversePin output always validates as a 4-digit pin", "[reverse][pin]") {
    for (std::uint32_t v = 0; v < reducer::kReversePinModulus; ++v) {
        const std::string pin = reducer::formatReversePin(v);
        REQUIRE(pin.size() == 4);
        REQUIRE(reducer::isValidReversePin(pin));
    }
}

TEST_CASE("isValidReversePin rejects the wrong length or non-digits", "[reverse][pin]") {
    REQUIRE(reducer::isValidReversePin("0000"));
    REQUIRE(reducer::isValidReversePin("9182"));
    REQUIRE_FALSE(reducer::isValidReversePin(""));
    REQUIRE_FALSE(reducer::isValidReversePin("123"));
    REQUIRE_FALSE(reducer::isValidReversePin("12345"));
    REQUIRE_FALSE(reducer::isValidReversePin("12a4"));
    REQUIRE_FALSE(reducer::isValidReversePin("12 4"));
    REQUIRE_FALSE(reducer::isValidReversePin("-123"));
}
