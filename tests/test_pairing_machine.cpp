// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The forward (user-types-a-PIN) pairing FSM, pinned for every (phase x event):
// the next phase, the retained typed PairFailure, and the carried or cleared pin.

#include "core/reducer/PairingMachine.h"
#include "core/reducer/RestOutcome.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dish::reducer;
namespace pe = dish::reducer::pair_event;

namespace {

PairingState state(PairPhase phase, std::optional<PairFailure> failure = std::nullopt,
                   std::string pin = std::string()) {
    PairingState s;
    s.phase = phase;
    s.failure = failure;
    s.pin = std::move(pin);
    return s;
}

} // namespace

TEST_CASE("idle + submit starts an attempt carrying the pin", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Idle), pe::Submit{"1234"});
    CHECK(r.phase == PairPhase::Submitting);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin == "1234");
}

TEST_CASE("idle + reply is a stale no-op", "[pair-fsm]") {
    for (const PairVerdict v :
         {PairVerdict::Success, PairVerdict::Pending, PairVerdict::AuthRequired,
          PairVerdict::VersionMismatch, PairVerdict::Unreachable}) {
        const auto r = reducePairing(state(PairPhase::Idle), pe::ReplyClassified{v});
        CHECK(r == state(PairPhase::Idle));
    }
}

TEST_CASE("idle + session-live is a stray no-op", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Idle), pe::SessionConfirmedLive{});
    CHECK(r == state(PairPhase::Idle));
}

TEST_CASE("idle + cancel stays idle", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Idle), pe::Cancel{});
    CHECK(r == state(PairPhase::Idle));
}

TEST_CASE("submitting + classified success stays submitting until the session is live",
          "[pair-fsm]") {
    // A Success verdict means "key adopted, session opening", not terminal
    // success, so the phase waits for SessionConfirmedLive instead of guessing.
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                 pe::ReplyClassified{PairVerdict::Success});
    CHECK(r.phase == PairPhase::Submitting);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin == "1234");
}

TEST_CASE("submitting + classified pending keeps waiting (not a terminal forward failure)",
          "[pair-fsm]") {
    // The forward path does not expect Pending, but must not collapse to Failed.
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                 pe::ReplyClassified{PairVerdict::Pending});
    CHECK(r.phase == PairPhase::Submitting);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin == "1234");
}

TEST_CASE("submitting + auth-required fails as wrong pin and retains it", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                 pe::ReplyClassified{PairVerdict::AuthRequired});
    CHECK(r.phase == PairPhase::Failed);
    REQUIRE(r.failure.has_value());
    CHECK(*r.failure == PairFailure::WrongPin);
    CHECK(r.pin == "1234"); // retained on Failed for the UI / a retry
}

TEST_CASE("submitting + version-mismatch fails with the version reason", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                 pe::ReplyClassified{PairVerdict::VersionMismatch});
    CHECK(r.phase == PairPhase::Failed);
    REQUIRE(r.failure.has_value());
    CHECK(*r.failure == PairFailure::VersionMismatch);
}

TEST_CASE("submitting + unreachable fails with the unreachable reason", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                 pe::ReplyClassified{PairVerdict::Unreachable});
    CHECK(r.phase == PairPhase::Failed);
    REQUIRE(r.failure.has_value());
    CHECK(*r.failure == PairFailure::Unreachable);
}

TEST_CASE("submitting + session-live succeeds and clears the pin", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                 pe::SessionConfirmedLive{});
    CHECK(r.phase == PairPhase::Succeeded);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin.empty()); // attempt done — pin dropped
}

TEST_CASE("submitting + re-submit adopts the newest pin", "[pair-fsm]") {
    const auto r =
        reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"), pe::Submit{"5678"});
    CHECK(r.phase == PairPhase::Submitting);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin == "5678");
}

TEST_CASE("submitting + cancel returns to idle", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"), pe::Cancel{});
    CHECK(r == state(PairPhase::Idle));
}

TEST_CASE("succeeded + submit starts a fresh attempt", "[pair-fsm]") {
    // The live session can drop later and the user re-pairs.
    const auto r = reducePairing(state(PairPhase::Succeeded), pe::Submit{"4321"});
    CHECK(r.phase == PairPhase::Submitting);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin == "4321");
}

TEST_CASE("succeeded + reply is a stale no-op", "[pair-fsm]") {
    for (const PairVerdict v :
         {PairVerdict::Success, PairVerdict::Pending, PairVerdict::AuthRequired,
          PairVerdict::VersionMismatch, PairVerdict::Unreachable}) {
        const auto r = reducePairing(state(PairPhase::Succeeded), pe::ReplyClassified{v});
        CHECK(r == state(PairPhase::Succeeded));
    }
}

TEST_CASE("succeeded + another session-live is a no-op", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Succeeded), pe::SessionConfirmedLive{});
    CHECK(r == state(PairPhase::Succeeded));
}

TEST_CASE("succeeded + cancel returns to idle", "[pair-fsm]") {
    const auto r = reducePairing(state(PairPhase::Succeeded), pe::Cancel{});
    CHECK(r == state(PairPhase::Idle));
}

TEST_CASE("failed + submit retries and clears the prior failure", "[pair-fsm]") {
    // Dropping the retained reason is what lets the UI leave the error state.
    const auto r =
        reducePairing(state(PairPhase::Failed, PairFailure::WrongPin, "1234"), pe::Submit{"5678"});
    CHECK(r.phase == PairPhase::Submitting);
    CHECK_FALSE(r.failure.has_value());
    CHECK(r.pin == "5678");
}

TEST_CASE("failed + reply is a stale no-op (preserves the retained reason)", "[pair-fsm]") {
    const auto failed = state(PairPhase::Failed, PairFailure::Unreachable, "1234");
    for (const PairVerdict v :
         {PairVerdict::Success, PairVerdict::Pending, PairVerdict::AuthRequired,
          PairVerdict::VersionMismatch, PairVerdict::Unreachable}) {
        const auto r = reducePairing(failed, pe::ReplyClassified{v});
        CHECK(r == failed);
    }
}

TEST_CASE("failed + session-live is a stray no-op", "[pair-fsm]") {
    const auto failed = state(PairPhase::Failed, PairFailure::WrongPin, "1234");
    const auto r = reducePairing(failed, pe::SessionConfirmedLive{});
    CHECK(r == failed);
}

TEST_CASE("failed + cancel returns to idle and drops the reason", "[pair-fsm]") {
    const auto r =
        reducePairing(state(PairPhase::Failed, PairFailure::VersionMismatch, "1234"), pe::Cancel{});
    CHECK(r == state(PairPhase::Idle));
}

TEST_CASE("each failing PairVerdict maps to its distinct PairFailure", "[pair-fsm]") {
    struct Case {
        PairVerdict verdict;
        PairFailure failure;
    };
    const Case cases[] = {
        {PairVerdict::AuthRequired, PairFailure::WrongPin},
        {PairVerdict::VersionMismatch, PairFailure::VersionMismatch},
        {PairVerdict::Unreachable, PairFailure::Unreachable},
    };
    for (const auto& c : cases) {
        const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                     pe::ReplyClassified{c.verdict});
        CHECK(r.phase == PairPhase::Failed);
        REQUIRE(r.failure.has_value());
        CHECK(*r.failure == c.failure);
    }
    // The two non-failing arms never reach Failed from Submitting.
    for (const PairVerdict v : {PairVerdict::Success, PairVerdict::Pending}) {
        const auto r = reducePairing(state(PairPhase::Submitting, std::nullopt, "1234"),
                                     pe::ReplyClassified{v});
        CHECK(r.phase == PairPhase::Submitting);
        CHECK_FALSE(r.failure.has_value());
    }
}

TEST_CASE("classifyPair drives the reply transition end-to-end", "[pair-fsm]") {
    auto submitThenReply = [](const PairReply& reply) {
        const auto submitting = reducePairing(state(PairPhase::Idle), pe::Submit{"1234"});
        return reducePairing(submitting, pe::ReplyClassified{classifyPair(reply)});
    };

    SECTION("ok + key -> Success keeps us Submitting (awaiting live session)") {
        PairReply reply;
        reply.status = 200;
        reply.bodyParsed = true;
        reply.ok = true;
        reply.hasSharedKey = true;
        const auto r = submitThenReply(reply);
        CHECK(r.phase == PairPhase::Submitting);
        CHECK_FALSE(r.failure.has_value());
    }

    SECTION("reachable, parsed, no key -> AuthRequired -> Failed(WrongPin)") {
        PairReply reply;
        reply.status = 200;
        reply.bodyParsed = true;
        reply.ok = false;
        reply.hasSharedKey = false;
        const auto r = submitThenReply(reply);
        CHECK(r.phase == PairPhase::Failed);
        REQUIRE(r.failure.has_value());
        CHECK(*r.failure == PairFailure::WrongPin);
    }

    SECTION("409 -> VersionMismatch -> Failed(VersionMismatch)") {
        PairReply reply;
        reply.status = 409;
        reply.bodyParsed = true;
        const auto r = submitThenReply(reply);
        CHECK(r.phase == PairPhase::Failed);
        REQUIRE(r.failure.has_value());
        CHECK(*r.failure == PairFailure::VersionMismatch);
    }

    SECTION("status 0 / empty body -> Unreachable -> Failed(Unreachable)") {
        PairReply reply;
        reply.status = 0;
        reply.bodyParsed = false;
        const auto r = submitThenReply(reply);
        CHECK(r.phase == PairPhase::Failed);
        REQUIRE(r.failure.has_value());
        CHECK(*r.failure == PairFailure::Unreachable);
    }

    SECTION("pending body -> Pending keeps us Submitting") {
        PairReply reply;
        reply.status = 200;
        reply.bodyParsed = true;
        reply.ok = false;
        reply.pending = true;
        const auto r = submitThenReply(reply);
        CHECK(r.phase == PairPhase::Submitting);
        CHECK_FALSE(r.failure.has_value());
    }
}

TEST_CASE("happy path: idle -> submit -> success-verdict -> session-live -> succeeded",
          "[pair-fsm]") {
    auto s = state(PairPhase::Idle);
    s = reducePairing(s, pe::Submit{"1234"});
    REQUIRE(s.phase == PairPhase::Submitting);
    s = reducePairing(s, pe::ReplyClassified{PairVerdict::Success});
    REQUIRE(s.phase == PairPhase::Submitting); // still opening, NOT yet succeeded
    s = reducePairing(s, pe::SessionConfirmedLive{});
    CHECK(s.phase == PairPhase::Succeeded);
    CHECK_FALSE(s.failure.has_value());
    CHECK(s.pin.empty());
}

TEST_CASE("cancel from every phase resets to a clean idle", "[pair-fsm]") {
    const PairingState states[] = {
        state(PairPhase::Idle),
        state(PairPhase::Submitting, std::nullopt, "1234"),
        state(PairPhase::Succeeded),
        state(PairPhase::Failed, PairFailure::WrongPin, "1234"),
    };
    for (const auto& s : states) {
        const auto r = reducePairing(s, pe::Cancel{});
        CHECK(r == state(PairPhase::Idle));
        CHECK_FALSE(r.failure.has_value());
        CHECK(r.pin.empty());
    }
}

TEST_CASE("reducePairing is total over every phase and event", "[pair-fsm]") {
    const PairEvent events[] = {
        pe::Submit{"1234"},
        pe::ReplyClassified{PairVerdict::Success},
        pe::ReplyClassified{PairVerdict::Pending},
        pe::ReplyClassified{PairVerdict::AuthRequired},
        pe::ReplyClassified{PairVerdict::VersionMismatch},
        pe::ReplyClassified{PairVerdict::Unreachable},
        pe::SessionConfirmedLive{},
        pe::Cancel{},
    };
    const PairPhase phases[] = {PairPhase::Idle, PairPhase::Submitting, PairPhase::Succeeded,
                                PairPhase::Failed};
    for (const PairPhase phase : phases) {
        const auto failure = phase == PairPhase::Failed
                                 ? std::optional<PairFailure>(PairFailure::WrongPin)
                                 : std::nullopt;
        for (const auto& e : events) {
            // The invariant every arm must hold: a failure is populated if and
            // only if the resulting phase is Failed.
            const auto r = reducePairing(state(phase, failure, "1234"), e);
            CHECK((r.failure.has_value() == (r.phase == PairPhase::Failed)));
        }
    }
}

TEST_CASE("starting an attempt always clears a stale failure", "[pair-fsm]") {
    for (const PairPhase phase :
         {PairPhase::Idle, PairPhase::Submitting, PairPhase::Succeeded, PairPhase::Failed}) {
        const auto failure = phase == PairPhase::Failed
                                 ? std::optional<PairFailure>(PairFailure::VersionMismatch)
                                 : std::nullopt;
        const auto r = reducePairing(state(phase, failure, "old"), pe::Submit{"new"});
        CHECK(r.phase == PairPhase::Submitting);
        CHECK_FALSE(r.failure.has_value());
        CHECK(r.pin == "new");
    }
}
