// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// TransportFailure is diagnostic, never an input to a decision. These pin that
// property down: the verdict a reply produces must be identical whatever cause
// is attached to it, so a future cause can never silently change retry policy.

#include "core/reducer/RestOutcome.h"

#include <catch2/catch_test_macros.hpp>

using namespace dish::reducer;

namespace {
constexpr TransportFailure kAll[] = {
    TransportFailure::None,     TransportFailure::Unreachable, TransportFailure::Refused,
    TransportFailure::TimedOut, TransportFailure::Tls,         TransportFailure::Aborted,
    TransportFailure::Other,
};
} // namespace

TEST_CASE("a transport cause never changes the REST verdict") {
    for (const auto failure : kAll) {
        RestReply statusless;
        statusless.failure = failure;
        REQUIRE(classifyRest(statusless) == RestVerdict::Unreachable);

        RestReply ok;
        ok.status = 200;
        ok.bodyParsed = true;
        ok.failure = failure;
        REQUIRE(classifyRest(ok) == RestVerdict::Ok);

        RestReply unauthorized;
        unauthorized.status = 401;
        unauthorized.bodyParsed = true;
        unauthorized.failure = failure;
        REQUIRE(classifyRest(unauthorized) == RestVerdict::Unauthorized);
    }
}

TEST_CASE("a pin mismatch still outranks every transport cause") {
    // The TOFU abort surfaces as Aborted, and an identity change must not be
    // downgraded to the ordinary offline path by it.
    RestReply r;
    r.pinMismatch = true;
    r.failure = TransportFailure::Aborted;
    REQUIRE(classifyRest(r) == RestVerdict::IdentityChanged);
    REQUIRE(restVerdictTerminal(classifyRest(r)));
}

TEST_CASE("only the two ordinary offline causes are routine") {
    // Routine causes repeat forever behind a switched-off satellite and are
    // logged once; everything else is unusual enough to log each time.
    REQUIRE(transportFailureIsRoutine(TransportFailure::Unreachable));
    REQUIRE(transportFailureIsRoutine(TransportFailure::Refused));

    REQUIRE_FALSE(transportFailureIsRoutine(TransportFailure::None));
    REQUIRE_FALSE(transportFailureIsRoutine(TransportFailure::TimedOut));
    REQUIRE_FALSE(transportFailureIsRoutine(TransportFailure::Tls));
    REQUIRE_FALSE(transportFailureIsRoutine(TransportFailure::Aborted));
    REQUIRE_FALSE(transportFailureIsRoutine(TransportFailure::Other));
}

TEST_CASE("a statusless reply defaults to no attributed cause") {
    // The gateway sets the cause only on the failure path, so the default has to
    // be the honest "we were not told", not a guess at unreachable.
    const RestReply fresh;
    REQUIRE(fresh.failure == TransportFailure::None);
}
