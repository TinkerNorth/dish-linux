// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// No timers and no sockets here: the wall clock reaches the machine as Tick
// events, so every timing rule below is exact rather than flaky.

#include "core/reducer/ApplyBindingMachine.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::applyCancellable;
using dish::reducer::ApplyFailure;
using dish::reducer::applyInFlight;
using dish::reducer::ApplyPhase;
using dish::reducer::ApplyState;
using dish::reducer::ApplyStepState;
using dish::reducer::reduceApply;
namespace ev = dish::reducer::apply_event;

namespace {

// A run parked on its Connection step, mid path switch.
ApplyState switching(bool wantsDirect = true) {
    return reduceApply(ApplyState{}, ev::Start{/*needsPathSwitch=*/true, wantsDirect});
}

// A run parked on its Destination step, path switch skipped.
ApplyState binding() {
    return reduceApply(ApplyState{}, ev::Start{/*needsPathSwitch=*/false, /*wantsDirect=*/false});
}

} // namespace

TEST_CASE("apply machine: a fresh state is idle and inert", "[apply][machine]") {
    const ApplyState s;
    REQUIRE(s.phase == ApplyPhase::Idle);
    REQUIRE(s.connection == ApplyStepState::Pending);
    REQUIRE(s.destination == ApplyStepState::Pending);
    REQUIRE_FALSE(applyInFlight(s));
    REQUIRE_FALSE(applyCancellable(s));
    REQUIRE_FALSE(s.failure.has_value());
}

TEST_CASE("apply machine: Start without a path switch skips the Connection step",
          "[apply][machine]") {
    const auto s = binding();
    REQUIRE(s.phase == ApplyPhase::Binding);
    // Skipped, not Done: the overlay must not claim to have done work it did not.
    REQUIRE(s.connection == ApplyStepState::Skipped);
    REQUIRE(s.destination == ApplyStepState::Active);
    REQUIRE(applyInFlight(s));
    REQUIRE_FALSE(applyCancellable(s));
}

TEST_CASE("apply machine: Start with a path switch activates the Connection step",
          "[apply][machine]") {
    const auto s = switching();
    REQUIRE(s.phase == ApplyPhase::SwitchingPath);
    REQUIRE(s.connection == ApplyStepState::Active);
    REQUIRE(s.destination == ApplyStepState::Pending);
    REQUIRE(applyInFlight(s));
    REQUIRE(applyCancellable(s));
    REQUIRE(s.elapsedMsOnStep == 0);
}

TEST_CASE("apply machine: a settled Direct claim hands off with no fallback", "[apply][machine]") {
    const auto s = reduceApply(switching(/*wantsDirect=*/true), ev::PathSettled{/*direct=*/true});
    REQUIRE(s.connection == ApplyStepState::Done);
    REQUIRE(s.phase == ApplyPhase::Binding);
    REQUIRE(s.destination == ApplyStepState::Active);
    REQUIRE_FALSE(s.directFellBack);
    REQUIRE(s.elapsedMsOnStep == 0);
}

TEST_CASE("apply machine: asking for Direct and landing on Standard is a fallback",
          "[apply][machine]") {
    const auto s = reduceApply(switching(/*wantsDirect=*/true), ev::PathSettled{/*direct=*/false});
    REQUIRE(s.phase == ApplyPhase::Binding);
    REQUIRE(s.connection == ApplyStepState::Done);
    REQUIRE(s.directFellBack);
    REQUIRE_FALSE(s.failure.has_value()); // a warning, not a failure
}

TEST_CASE("apply machine: switching back to Standard on purpose is not a fallback",
          "[apply][machine]") {
    const auto s = reduceApply(switching(/*wantsDirect=*/false), ev::PathSettled{/*direct=*/false});
    REQUIRE(s.phase == ApplyPhase::Binding);
    REQUIRE_FALSE(s.directFellBack);
}

TEST_CASE("apply machine: a claim that times out continues to the bind", "[apply][machine]") {
    // The claim expiring only means the OS never handed the device over; the pad
    // still streams over Standard, so this is a warning and the run goes on.
    const auto s = reduceApply(switching(), ev::PathTimedOut{});
    REQUIRE(s.phase == ApplyPhase::Binding);
    REQUIRE(s.connection == ApplyStepState::Done);
    REQUIRE(s.destination == ApplyStepState::Active);
    REQUIRE(s.directFellBack);
    REQUIRE_FALSE(s.failure.has_value());
}

TEST_CASE("apply machine: an accepted bind succeeds", "[apply][machine]") {
    const auto s = reduceApply(binding(), ev::BindAccepted{});
    REQUIRE(s.phase == ApplyPhase::Succeeded);
    REQUIRE(s.destination == ApplyStepState::Done);
    REQUIRE_FALSE(applyInFlight(s));
    REQUIRE_FALSE(s.failure.has_value());
}

TEST_CASE("apply machine: a rejected bind reports the satellite refusing it", "[apply][machine]") {
    const auto s = reduceApply(binding(), ev::BindRejected{/*unreachable=*/false});
    REQUIRE(s.phase == ApplyPhase::Failed);
    REQUIRE(s.destination == ApplyStepState::Failed);
    REQUIRE(s.failure == ApplyFailure::BindRejected);
}

TEST_CASE("apply machine: an unreachable host is distinct from a refusal", "[apply][machine]") {
    const auto rejected = reduceApply(binding(), ev::BindRejected{/*unreachable=*/true});
    REQUIRE(rejected.failure == ApplyFailure::HostUnreachable);
    // A round-trip that never answers folds into the same HostUnreachable reason.
    const auto timedOut = reduceApply(binding(), ev::BindTimedOut{});
    REQUIRE(timedOut.phase == ApplyPhase::Failed);
    REQUIRE(timedOut.destination == ApplyStepState::Failed);
    REQUIRE(timedOut.failure == ApplyFailure::HostUnreachable);
}

TEST_CASE("apply machine: Cancel is accepted only while the claim is in flight",
          "[apply][machine]") {
    SECTION("during the Connection step") {
        const auto s = reduceApply(switching(), ev::Cancel{});
        REQUIRE(s.phase == ApplyPhase::Cancelled);
        REQUIRE(s.connection == ApplyStepState::Failed);
        REQUIRE(s.destination == ApplyStepState::Pending);
        REQUIRE(s.failure == ApplyFailure::Cancelled);
    }
    SECTION("during the Destination step it is refused") {
        const auto before = binding();
        const auto after = reduceApply(before, ev::Cancel{});
        REQUIRE(after.phase == ApplyPhase::Binding);
        REQUIRE_FALSE(after.failure.has_value());
    }
    SECTION("from Idle it is refused") {
        const auto after = reduceApply(ApplyState{}, ev::Cancel{});
        REQUIRE(after.phase == ApplyPhase::Idle);
        REQUIRE_FALSE(after.failure.has_value());
    }
    SECTION("after success it is refused") {
        const auto done = reduceApply(binding(), ev::BindAccepted{});
        const auto after = reduceApply(done, ev::Cancel{});
        REQUIRE(after.phase == ApplyPhase::Succeeded);
    }
}

TEST_CASE("apply machine: the pad vanishing is terminal from every live phase",
          "[apply][machine]") {
    SECTION("from Idle") {
        const auto s = reduceApply(ApplyState{}, ev::SlotVanished{});
        REQUIRE(s.phase == ApplyPhase::Failed);
        REQUIRE(s.failure == ApplyFailure::SlotGone);
    }
    SECTION("from the Connection step") {
        const auto s = reduceApply(switching(), ev::SlotVanished{});
        REQUIRE(s.phase == ApplyPhase::Failed);
        REQUIRE(s.failure == ApplyFailure::SlotGone);
        REQUIRE(s.connection == ApplyStepState::Failed);
    }
    SECTION("from the Destination step") {
        const auto s = reduceApply(binding(), ev::SlotVanished{});
        REQUIRE(s.phase == ApplyPhase::Failed);
        REQUIRE(s.failure == ApplyFailure::SlotGone);
        REQUIRE(s.destination == ApplyStepState::Failed);
        REQUIRE(s.connection == ApplyStepState::Skipped); // untouched
    }
    SECTION("a finished run keeps its result") {
        const auto done = reduceApply(binding(), ev::BindAccepted{});
        const auto after = reduceApply(done, ev::SlotVanished{});
        REQUIRE(after.phase == ApplyPhase::Succeeded);
        REQUIRE_FALSE(after.failure.has_value());
    }
}

TEST_CASE("apply machine: the elapsed clock only runs while a step is in flight",
          "[apply][machine]") {
    auto s = switching();
    s = reduceApply(s, ev::Tick{250});
    s = reduceApply(s, ev::Tick{250});
    REQUIRE(s.elapsedMsOnStep == 500);

    // Crossing a step boundary restarts it: the 4 s slow hint is per step.
    s = reduceApply(s, ev::PathSettled{/*direct=*/true});
    REQUIRE(s.elapsedMsOnStep == 0);
    s = reduceApply(s, ev::Tick{4000});
    REQUIRE(s.elapsedMsOnStep == 4000);

    s = reduceApply(s, ev::BindAccepted{});
    const int frozen = s.elapsedMsOnStep;
    s = reduceApply(s, ev::Tick{1000});
    REQUIRE(s.elapsedMsOnStep == frozen);
}

TEST_CASE("apply machine: out-of-phase step events are ignored", "[apply][machine]") {
    // The production timers are real and can fire late; a stale budget must not
    // corrupt a run that has already moved on.
    const auto bindingState = binding();
    REQUIRE(reduceApply(bindingState, ev::PathSettled{true}).phase == ApplyPhase::Binding);
    REQUIRE(reduceApply(bindingState, ev::PathTimedOut{}).phase == ApplyPhase::Binding);
    REQUIRE_FALSE(reduceApply(bindingState, ev::PathTimedOut{}).directFellBack);

    const auto switchingState = switching();
    REQUIRE(reduceApply(switchingState, ev::BindAccepted{}).phase == ApplyPhase::SwitchingPath);
    REQUIRE(reduceApply(switchingState, ev::BindTimedOut{}).phase == ApplyPhase::SwitchingPath);
    REQUIRE(reduceApply(switchingState, ev::BindRejected{}).phase == ApplyPhase::SwitchingPath);
}

TEST_CASE("apply machine: Start restarts a failed run cleanly", "[apply][machine]") {
    // Failed leaves the draft intact and the primary live again, so the next
    // event a run can legitimately see is another Start.
    const auto failed = reduceApply(binding(), ev::BindRejected{});
    REQUIRE(failed.phase == ApplyPhase::Failed);

    const auto retried = reduceApply(failed, ev::Start{/*needsPathSwitch=*/true,
                                                       /*wantsDirect=*/true});
    REQUIRE(retried.phase == ApplyPhase::SwitchingPath);
    REQUIRE(retried.connection == ApplyStepState::Active);
    REQUIRE(retried.destination == ApplyStepState::Pending);
    REQUIRE_FALSE(retried.failure.has_value());
    REQUIRE_FALSE(retried.directFellBack);
    REQUIRE(retried.elapsedMsOnStep == 0);
}

TEST_CASE("apply machine: the full happy path with a claim runs Connection then Destination",
          "[apply][machine]") {
    auto s = reduceApply(ApplyState{}, ev::Start{/*needsPathSwitch=*/true, /*wantsDirect=*/true});
    REQUIRE(s.connection == ApplyStepState::Active);
    s = reduceApply(s, ev::Tick{4000}); // the slow-hint threshold
    REQUIRE(s.elapsedMsOnStep == 4000);
    REQUIRE(applyCancellable(s));
    s = reduceApply(s, ev::PathSettled{/*direct=*/true});
    REQUIRE(s.connection == ApplyStepState::Done);
    REQUIRE(s.destination == ApplyStepState::Active);
    REQUIRE_FALSE(applyCancellable(s));
    s = reduceApply(s, ev::BindAccepted{});
    REQUIRE(s.phase == ApplyPhase::Succeeded);
    REQUIRE(s.connection == ApplyStepState::Done);
    REQUIRE(s.destination == ApplyStepState::Done);
    REQUIRE_FALSE(s.directFellBack);
}
