// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SESSION_CLOSE (0x000F) reason byte → teardown action, and the reason-byte
// values themselves (contract §UDP messages / §Session-close notify).

#include "Models/Protocol.h"
#include "Network/CloseNotify.h"

#include <catch2/catch_test_macros.hpp>

namespace proto = dish::proto;
namespace reducer = dish::reducer;

TEST_CASE("close-notify reason maps to the right teardown action", "[close]") {
    using reducer::CloseAction;
    // unpaired: trust revoked — drop the key and stop retrying.
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonUnpaired) ==
            CloseAction::DropKeyRePair);
    // replaced: a newer PUT owns the session — stay down.
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonReplaced) == CloseAction::StayDown);
    // shutdown / kicked: transient — reconnect on the backoff curve.
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonShutdown) ==
            CloseAction::RetryBackoff);
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonKicked) == CloseAction::RetryBackoff);
}

TEST_CASE("an unknown future reason byte degrades to a transient retry", "[close]") {
    REQUIRE(reducer::closeActionForReason(0x7F) == reducer::CloseAction::RetryBackoff);
}

TEST_CASE("close-notify reason bytes match the contract", "[close]") {
    REQUIRE(proto::kCloseReasonShutdown == 0);
    REQUIRE(proto::kCloseReasonKicked == 1);
    REQUIRE(proto::kCloseReasonReplaced == 2);
    REQUIRE(proto::kCloseReasonUnpaired == 3);
    REQUIRE(reducer::closeReasonName(proto::kCloseReasonUnpaired) == "unpaired");
    REQUIRE(reducer::closeReasonName(proto::kCloseReasonKicked) == "kicked");
    REQUIRE(reducer::closeReasonName(proto::kCloseReasonReplaced) == "replaced");
    REQUIRE(reducer::closeReasonName(proto::kCloseReasonShutdown) == "shutdown");
}
