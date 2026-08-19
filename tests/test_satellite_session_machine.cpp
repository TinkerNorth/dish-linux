// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The total per-satellite session FSM: for every (phase x event) the next phase,
// the retained typed failure, the carried retry/heartbeat fields, and the exact
// ordered effect list.

#include "core/model/Protocol.h"
#include "core/reducer/Backoff.h"
#include "core/reducer/CloseNotify.h"
#include "core/reducer/RestOutcome.h"
#include "core/reducer/SatelliteLinkState.h"
#include "core/reducer/SatelliteSessionMachine.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

using namespace dish::reducer;
namespace se = dish::reducer::session_event;
namespace sf = dish::reducer::session_effect;
namespace proto = dish::proto; // close-reason byte constants (kCloseReason*)

namespace {

SessionModel model(SessionPhase phase, ConnectIntent intent = ConnectIntent::UserInitiated,
                   int retryAttempt = 0, int missed = 0,
                   std::optional<SessionFailure> failure = std::nullopt,
                   long long nextRetryAtMs = 0) {
    SessionModel m;
    m.phase = phase;
    m.intent = intent;
    m.retryAttempt = retryAttempt;
    m.missedHeartbeats = missed;
    m.failure = failure;
    m.nextRetryAtMs = nextRetryAtMs;
    return m;
}

} // namespace

TEST_CASE("discovered + user connect (have key) starts the session PUT", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Discovered),
                          se::Connect{ConnectIntent::UserInitiated, /*needsPair=*/false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    CHECK(r.next->intent == ConnectIntent::UserInitiated);
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::OpenSession{}};
    CHECK(r.effects == expected);
}

TEST_CASE("discovered + connect needing a pair starts the pair handshake first", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Discovered),
                          se::Connect{ConnectIntent::UserInitiated, /*needsPair=*/true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Pairing);
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::Pair{}};
    CHECK(r.effects == expected);
}

TEST_CASE("discovered + auto connect adopts the silent intent", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Discovered),
                          se::Connect{ConnectIntent::AutoReconnect, /*needsPair=*/false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    CHECK(r.next->intent == ConnectIntent::AutoReconnect);
}

TEST_CASE("discovered ignores stray non-connect events", "[session-fsm]") {
    const auto base = model(SessionPhase::Discovered);
    CHECK(reduce(base, se::Linked{}).next == base);
    CHECK(reduce(base, se::RestClassified{RestVerdict::Ok}).next == base);
    CHECK(reduce(base, se::HeartbeatMiss{5}).next == base);
    CHECK(reduce(base, se::HeartbeatOk{}).next == base);
    CHECK(reduce(base, se::RetryTimerFired{}).next == base);
    CHECK(reduce(base, se::ReconcileDrift{}).next == base);
    CHECK(reduce(base, se::Closed{CloseAction::RetryBackoff}).next == base);
    CHECK(reduce(base, se::Linked{}).effects.empty());
}

TEST_CASE("pairing + pair success opens the session", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Pairing), se::PairClassified{PairVerdict::Success});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    const std::vector<SessionEffect> expected{sf::OpenSession{}};
    CHECK(r.effects == expected);
}

TEST_CASE("pairing + pending keeps waiting on the staged grant", "[session-fsm]") {
    const auto in = model(SessionPhase::Pairing);
    const auto r = reduce(in, se::PairClassified{PairVerdict::Pending});
    CHECK(r.next == in);
    CHECK(r.effects.empty());
}

TEST_CASE("pairing + auth-required is a terminal declined failure", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Pairing), se::PairClassified{PairVerdict::AuthRequired});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Failed);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::Declined);
    const std::vector<SessionEffect> expected{sf::Notify{SessionFailure::Declined, /*loud=*/true}};
    CHECK(r.effects == expected);
}

TEST_CASE("pairing + version-mismatch is terminal", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Pairing), se::PairClassified{PairVerdict::VersionMismatch});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Failed);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::VersionMismatch);
    const std::vector<SessionEffect> expected{
        sf::Notify{SessionFailure::VersionMismatch, /*loud=*/true}};
    CHECK(r.effects == expected);
}

TEST_CASE("pairing + unreachable rides the backoff curve", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Pairing), se::PairClassified{PairVerdict::Unreachable});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    CHECK(r.next->retryAttempt == 1);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::Unreachable);
    const std::vector<SessionEffect> expected{
        sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))},
        sf::Notify{SessionFailure::Unreachable, /*loud=*/true}};
    CHECK(r.effects == expected);
}

TEST_CASE("pairing ignores a session-PUT verdict (stale)", "[session-fsm]") {
    const auto in = model(SessionPhase::Pairing);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Ok}).next == in);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Unauthorized}).next == in);
}

TEST_CASE("linking + rest Ok stays linking until the socket binds", "[session-fsm]") {
    // Ok alone is not live: the UDP socket still has to open, which Linked reports.
    const auto in = model(SessionPhase::Linking);
    const auto r = reduce(in, se::RestClassified{RestVerdict::Ok});
    CHECK(r.next == in);
    CHECK(r.effects.empty());
}

TEST_CASE("linking + linked goes live, clears failure, starts the heartbeat", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Linking, ConnectIntent::UserInitiated,
                                /*retryAttempt=*/3, /*missed=*/0,
                                /*failure=*/SessionFailure::Unreachable),
                          se::Linked{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Live);
    CHECK_FALSE(r.next->failure.has_value());
    CHECK(r.next->retryAttempt == 0); // success RESETS the backoff curve
    CHECK(r.next->missedHeartbeats == 0);
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::StartHeartbeat{}};
    CHECK(r.effects == expected);
}

TEST_CASE("linking + 401 is terminal AuthRejected and drops the key", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Linking), se::RestClassified{RestVerdict::Unauthorized});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Failed);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::AuthRejected);
    const std::vector<SessionEffect> expected{
        sf::DropKey{}, sf::Notify{SessionFailure::AuthRejected, /*loud=*/true}};
    CHECK(r.effects == expected);
}

TEST_CASE("linking + 409 is terminal VersionMismatch and KEEPS the key", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Linking), se::RestClassified{RestVerdict::VersionMismatch});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Failed);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::VersionMismatch);
    // No DropKey: a protocol skew is not a trust failure.
    const std::vector<SessionEffect> expected{
        sf::Notify{SessionFailure::VersionMismatch, /*loud=*/true}};
    CHECK(r.effects == expected);
}

TEST_CASE("linking + unreachable schedules a backoff retry (loud when user-initiated)",
          "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Linking, ConnectIntent::UserInitiated),
                          se::RestClassified{RestVerdict::Unreachable});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    CHECK(r.next->retryAttempt == 1);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::Unreachable);
    const std::vector<SessionEffect> expected{
        sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))},
        sf::Notify{SessionFailure::Unreachable, /*loud=*/true}};
    CHECK(r.effects == expected);
}

TEST_CASE("linking + unreachable on an auto-reconnect is SILENT", "[session-fsm]") {
    // The same transient failure under a silent intent still notifies, with
    // loud == false, so no toast pops.
    const auto r = reduce(model(SessionPhase::Linking, ConnectIntent::AutoReconnect),
                          se::RestClassified{RestVerdict::Unreachable});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    const std::vector<SessionEffect> expected{
        sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))},
        sf::Notify{SessionFailure::Unreachable, /*loud=*/false}};
    CHECK(r.effects == expected);
}

TEST_CASE("linking + 503 reconnects with the shutting-down reason", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Linking), se::RestClassified{RestVerdict::ShuttingDown});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::ServerShuttingDown);
}

TEST_CASE("linking + server-error reconnects with the server-error reason", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Linking), se::RestClassified{RestVerdict::ServerError});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::ServerError);
}

TEST_CASE("linking ignores a pair verdict (stale)", "[session-fsm]") {
    const auto in = model(SessionPhase::Linking);
    CHECK(reduce(in, se::PairClassified{PairVerdict::Success}).next == in);
}

TEST_CASE("live + heartbeat miss at the not-responding count enters Faltering (THE BUG FIX)",
          "[session-fsm]") {
    // No effects: the socket is still up, only the chip degrades.
    const auto r =
        reduce(model(SessionPhase::Live), se::HeartbeatMiss{kHeartbeatMissNotResponding});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Faltering);
    CHECK(r.next->missedHeartbeats == kHeartbeatMissNotResponding);
    CHECK(r.effects.empty());
    CHECK(sessionPhaseToPresence(r.next->phase) == SessionPresence::Faltering);
    CHECK(satelliteLinkState(SessionPresence::Faltering, false, true) == UiLinkState::Unstable);
}

TEST_CASE("live + a sub-threshold miss just records the count", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live), se::HeartbeatMiss{1});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Live);
    CHECK(r.next->missedHeartbeats == 1);
    CHECK(r.effects.empty());
}

TEST_CASE("live + heartbeat miss at the dead count reconnects with a scheduled retry",
          "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live), se::HeartbeatMiss{kHeartbeatMissDead});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    CHECK(r.next->retryAttempt == 1);
    CHECK(r.next->missedHeartbeats == kHeartbeatMissDead);
    // A heartbeat death has no user tap behind it, so nothing is notified.
    const std::vector<SessionEffect> expected{
        sf::StopHeartbeat{}, sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))}};
    CHECK(r.effects == expected);
}

TEST_CASE("live + heartbeat ok is a no-op beyond resetting misses", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live, ConnectIntent::UserInitiated, 0, /*missed=*/1),
                          se::HeartbeatOk{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Live);
    CHECK(r.next->missedHeartbeats == 0);
    CHECK(r.effects.empty());
}

TEST_CASE("live + reconcile drift converges via a fresh session PUT", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Live, ConnectIntent::AutoReconnect, /*retryAttempt=*/2),
               se::ReconcileDrift{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    CHECK(r.next->retryAttempt == 2); // a reconcile is not a failure
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<SessionEffect> expected{sf::StopHeartbeat{}, sf::OpenSession{}};
    CHECK(r.effects == expected);
}

TEST_CASE("live + close(unpaired) parks Stale with the cause and drops the key", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live),
                          se::Closed{closeActionForReason(proto::kCloseReasonUnpaired)});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Stale); // not Failed, but it keeps a cause
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::AuthRejected);
    const std::vector<SessionEffect> expected{sf::StopHeartbeat{}, sf::DropKey{}};
    CHECK(r.effects == expected);
}

TEST_CASE("live + close(replaced) stays down, key kept", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live),
                          se::Closed{closeActionForReason(proto::kCloseReasonReplaced)});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Stale);
    CHECK_FALSE(r.next->failure.has_value()); // replaced is not a failure cause
    const std::vector<SessionEffect> expected{sf::StopHeartbeat{}};
    CHECK(r.effects == expected);
}

TEST_CASE("live + close(shutdown) reconnects on the backoff", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live),
                          se::Closed{closeActionForReason(proto::kCloseReasonShutdown)});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    CHECK(r.next->retryAttempt == 1);
    // A transient close is a silent retry: no notify.
    const std::vector<SessionEffect> expected{
        sf::StopHeartbeat{}, sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))}};
    CHECK(r.effects == expected);
}

TEST_CASE("live + connect is a no-op (already connected)", "[session-fsm]") {
    const auto in = model(SessionPhase::Live);
    const auto r = reduce(in, se::Connect{ConnectIntent::UserInitiated, false});
    CHECK(r.next == in);
    CHECK(r.effects.empty());
}

TEST_CASE("live ignores a stale session-PUT verdict", "[session-fsm]") {
    const auto in = model(SessionPhase::Live);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Ok}).next == in);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Unauthorized}).next == in);
}

TEST_CASE("faltering + heartbeat ok recovers to Live", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Faltering, ConnectIntent::UserInitiated, 0,
                                /*missed=*/kHeartbeatMissNotResponding),
                          se::HeartbeatOk{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Live);
    CHECK(r.next->missedHeartbeats == 0);
    CHECK(r.effects.empty());
}

TEST_CASE("faltering + more misses crossing the dead count reconnects", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Faltering, ConnectIntent::UserInitiated, 0,
                                /*missed=*/kHeartbeatMissNotResponding),
                          se::HeartbeatMiss{kHeartbeatMissDead});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    CHECK(r.next->retryAttempt == 1);
    const std::vector<SessionEffect> expected{
        sf::StopHeartbeat{}, sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))}};
    CHECK(r.effects == expected);
}

TEST_CASE("faltering + a still-degraded miss stays faltering", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Faltering), se::HeartbeatMiss{3});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Faltering);
    CHECK(r.next->missedHeartbeats == 3);
    CHECK(r.effects.empty());
}

TEST_CASE("faltering + linked re-asserts Live", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Faltering, ConnectIntent::UserInitiated, 0,
                                /*missed=*/kHeartbeatMissNotResponding),
                          se::Linked{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Live);
    CHECK(r.next->missedHeartbeats == 0);
}

TEST_CASE("faltering + close(shutdown) reconnects", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Faltering),
                          se::Closed{closeActionForReason(proto::kCloseReasonShutdown)});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Reconnecting);
    const std::vector<SessionEffect> expected{
        sf::StopHeartbeat{}, sf::ScheduleRetry{static_cast<int>(backoffDelayMs(1))}};
    CHECK(r.effects == expected);
}

TEST_CASE("reconnecting + retry timer fires re-attempts the session PUT", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect,
                                /*retryAttempt=*/3, /*missed=*/0,
                                /*failure=*/SessionFailure::Unreachable, /*nextRetryAtMs=*/99999),
                          se::RetryTimerFired{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    CHECK(r.next->retryAttempt == 3);
    CHECK(r.next->nextRetryAtMs == 0); // the pending deadline is consumed
    const std::vector<SessionEffect> expected{sf::OpenSession{}};
    CHECK(r.effects == expected);
}

TEST_CASE("reconnecting + rest Ok stays put until the socket binds", "[session-fsm]") {
    // The retry's PUT can land before the socket, so Ok must not jump to Live.
    const auto in = model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect, 2);
    const auto r = reduce(in, se::RestClassified{RestVerdict::Ok});
    CHECK(r.next == in);
    CHECK(r.effects.empty());
}

TEST_CASE("reconnecting + linked goes live and RESETS the retry count", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect,
                                /*retryAttempt=*/4, /*missed=*/0,
                                /*failure=*/SessionFailure::Unreachable),
                          se::Linked{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Live);
    CHECK(r.next->retryAttempt == 0);
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::StartHeartbeat{}};
    CHECK(r.effects == expected);
}

TEST_CASE("reconnecting + repeated transient failures grow the backoff delay", "[session-fsm]") {
    // The scheduled delay tracks the INCREMENTED attempt, not the old one.
    auto m = model(SessionPhase::Linking, ConnectIntent::AutoReconnect, /*retryAttempt=*/0);
    for (int expectAttempt = 1; expectAttempt <= 4; ++expectAttempt) {
        const auto r = reduce(m, se::RestClassified{RestVerdict::Unreachable});
        REQUIRE(r.next.has_value());
        CHECK(r.next->phase == SessionPhase::Reconnecting);
        CHECK(r.next->retryAttempt == expectAttempt);
        const std::vector<SessionEffect> expected{
            sf::ScheduleRetry{static_cast<int>(backoffDelayMs(expectAttempt))},
            sf::Notify{SessionFailure::Unreachable, /*loud=*/false}};
        CHECK(r.effects == expected);
        // The timer fires: back to Linking, ready for the next failure.
        m = *reduce(*r.next, se::RetryTimerFired{}).next;
        CHECK(m.retryAttempt == expectAttempt);
    }
}

TEST_CASE("reconnecting + user connect resets the curve to attempt 0", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect, /*retryAttempt=*/5),
               se::Connect{ConnectIntent::UserInitiated, /*needsPair=*/false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    CHECK(r.next->retryAttempt == 0);
    CHECK(r.next->intent == ConnectIntent::UserInitiated);
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::OpenSession{}};
    CHECK(r.effects == expected);
}

TEST_CASE("reconnecting + auto connect preserves the running count", "[session-fsm]") {
    const auto r =
        reduce(model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect, /*retryAttempt=*/5),
               se::Connect{ConnectIntent::AutoReconnect, /*needsPair=*/false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    CHECK(r.next->retryAttempt == 5);
}

TEST_CASE("reconnecting + heartbeat events are ignored (no live socket)", "[session-fsm]") {
    const auto in = model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect, 2);
    CHECK(reduce(in, se::HeartbeatMiss{kHeartbeatMissDead}).next == in);
    CHECK(reduce(in, se::HeartbeatOk{}).next == in);
    CHECK(reduce(in, se::ReconcileDrift{}).next == in);
}

TEST_CASE("stale + user connect re-links (the kept key)", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Stale),
                          se::Connect{ConnectIntent::UserInitiated, /*needsPair=*/false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Linking);
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::OpenSession{}};
    CHECK(r.effects == expected);
}

TEST_CASE("stale ignores stray late events", "[session-fsm]") {
    const auto in = model(SessionPhase::Stale, ConnectIntent::AutoReconnect, 0, 0,
                          SessionFailure::AuthRejected);
    CHECK(reduce(in, se::Linked{}).next == in);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Ok}).next == in);
    CHECK(reduce(in, se::RetryTimerFired{}).next == in);
    CHECK(reduce(in, se::HeartbeatMiss{5}).next == in);
}

TEST_CASE("failed + user connect retries and clears the prior failure", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Failed, ConnectIntent::UserInitiated, 0, 0,
                                SessionFailure::AuthRejected),
                          se::Connect{ConnectIntent::UserInitiated, /*needsPair=*/true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Pairing);
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<SessionEffect> expected{sf::ClearFailure{}, sf::Pair{}};
    CHECK(r.effects == expected);
}

TEST_CASE("failed ignores stale replies (preserves the retained reason)", "[session-fsm]") {
    const auto in = model(SessionPhase::Failed, ConnectIntent::UserInitiated, 0, 0,
                          SessionFailure::VersionMismatch);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Ok}).next == in);
    CHECK(reduce(in, se::RestClassified{RestVerdict::Unauthorized}).next == in);
    CHECK(reduce(in, se::PairClassified{PairVerdict::Success}).next == in);
    CHECK(reduce(in, se::Linked{}).next == in);
    CHECK(reduce(in, se::RetryTimerFired{}).next == in);
}

TEST_CASE("disconnect from live lands in Stale with the key KEPT", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live), se::Disconnect{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Stale);
    CHECK_FALSE(r.next->failure.has_value()); // graceful, so no cause
    // No DropKey: the key survives a graceful disconnect.
    const std::vector<SessionEffect> expected{sf::StopHeartbeat{}};
    CHECK(r.effects == expected);
}

TEST_CASE("disconnect from a non-live phase is still a clean Stale (no heartbeat to stop)",
          "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Reconnecting, ConnectIntent::AutoReconnect, 3),
                          se::Disconnect{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Stale);
    CHECK(r.next->retryAttempt == 0);
    CHECK(r.effects.empty()); // not live -> no StopHeartbeat
}

TEST_CASE("forget from live removes the session and drops the key", "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Live), se::Forget{});
    CHECK_FALSE(r.next.has_value());
    const std::vector<SessionEffect> expected{sf::StopHeartbeat{}, sf::DropKey{}};
    CHECK(r.effects == expected);
}

TEST_CASE("forget from a non-live phase removes it and drops the key without StopHeartbeat",
          "[session-fsm]") {
    const auto r = reduce(model(SessionPhase::Stale, ConnectIntent::AutoReconnect, 0, 0,
                                SessionFailure::AuthRejected),
                          se::Forget{});
    CHECK_FALSE(r.next.has_value());
    const std::vector<SessionEffect> expected{sf::DropKey{}};
    CHECK(r.effects == expected);
}

TEST_CASE("classifyRest drives the session-PUT transition end-to-end", "[session-fsm]") {
    auto linkThenReply = [](const RestReply& reply) {
        const auto linking = reduce(model(SessionPhase::Discovered),
                                    se::Connect{ConnectIntent::UserInitiated, false});
        return reduce(*linking.next, se::RestClassified{classifyRest(reply)});
    };

    SECTION("2xx -> Ok keeps us Linking (awaiting the socket)") {
        RestReply reply;
        reply.status = 200;
        reply.bodyParsed = true;
        const auto r = linkThenReply(reply);
        CHECK(r.next->phase == SessionPhase::Linking);
    }
    SECTION("401 -> Unauthorized -> terminal Failed(AuthRejected) + DropKey") {
        RestReply reply;
        reply.status = 401;
        reply.bodyParsed = true;
        reply.code = "NOT_PAIRED";
        const auto r = linkThenReply(reply);
        CHECK(r.next->phase == SessionPhase::Failed);
        REQUIRE(r.next->failure.has_value());
        CHECK(*r.next->failure == SessionFailure::AuthRejected);
    }
    SECTION("409 -> VersionMismatch -> terminal Failed(VersionMismatch)") {
        RestReply reply;
        reply.status = 409;
        reply.bodyParsed = true;
        const auto r = linkThenReply(reply);
        CHECK(r.next->phase == SessionPhase::Failed);
        CHECK(*r.next->failure == SessionFailure::VersionMismatch);
    }
    SECTION("status 0 -> Unreachable -> Reconnecting") {
        RestReply reply;
        reply.status = 0;
        reply.bodyParsed = false;
        const auto r = linkThenReply(reply);
        CHECK(r.next->phase == SessionPhase::Reconnecting);
        CHECK(*r.next->failure == SessionFailure::Unreachable);
    }
}

TEST_CASE("closeActionForReason drives the close transition end-to-end", "[session-fsm]") {
    auto closeFromLive = [](std::uint8_t reason) {
        return reduce(model(SessionPhase::Live), se::Closed{closeActionForReason(reason)});
    };
    CHECK(closeFromLive(proto::kCloseReasonUnpaired).next->phase == SessionPhase::Stale);
    CHECK(*closeFromLive(proto::kCloseReasonUnpaired).next->failure ==
          SessionFailure::AuthRejected);
    CHECK(closeFromLive(proto::kCloseReasonReplaced).next->phase == SessionPhase::Stale);
    CHECK(closeFromLive(proto::kCloseReasonShutdown).next->phase == SessionPhase::Reconnecting);
    CHECK(closeFromLive(proto::kCloseReasonKicked).next->phase == SessionPhase::Reconnecting);
}

TEST_CASE("happy path: discovered -> connect -> rest Ok -> linked -> live", "[session-fsm]") {
    auto s = model(SessionPhase::Discovered);
    s = *reduce(s, se::Connect{ConnectIntent::UserInitiated, false}).next;
    REQUIRE(s.phase == SessionPhase::Linking);
    s = *reduce(s, se::RestClassified{RestVerdict::Ok}).next;
    REQUIRE(s.phase == SessionPhase::Linking); // Ok alone is not yet live
    s = *reduce(s, se::Linked{}).next;
    CHECK(s.phase == SessionPhase::Live);
    CHECK(s.retryAttempt == 0);
    CHECK_FALSE(s.failure.has_value());
}

TEST_CASE("stutter-then-recover: live -> faltering -> live (no reconnect)", "[session-fsm]") {
    auto s = model(SessionPhase::Live);
    s = *reduce(s, se::HeartbeatMiss{kHeartbeatMissNotResponding}).next;
    REQUIRE(s.phase == SessionPhase::Faltering);
    s = *reduce(s, se::HeartbeatOk{}).next;
    CHECK(s.phase == SessionPhase::Live);
    CHECK(s.missedHeartbeats == 0);
}

TEST_CASE("death-then-retry-then-recover: live -> reconnecting -> linking -> live",
          "[session-fsm]") {
    auto s = model(SessionPhase::Live);
    s = *reduce(s, se::HeartbeatMiss{kHeartbeatMissDead}).next;
    REQUIRE(s.phase == SessionPhase::Reconnecting);
    REQUIRE(s.retryAttempt == 1);
    s = *reduce(s, se::RetryTimerFired{}).next;
    REQUIRE(s.phase == SessionPhase::Linking);
    REQUIRE(s.retryAttempt == 1);
    s = *reduce(s, se::Linked{}).next;
    CHECK(s.phase == SessionPhase::Live);
    CHECK(s.retryAttempt == 0); // only success resets the curve
}

TEST_CASE("every phase maps onto a SatelliteLinkState presence", "[session-fsm]") {
    CHECK(sessionPhaseToPresence(SessionPhase::Discovered) == SessionPresence::Idle);
    CHECK(sessionPhaseToPresence(SessionPhase::Pairing) == SessionPresence::Linking);
    CHECK(sessionPhaseToPresence(SessionPhase::Linking) == SessionPresence::Linking);
    CHECK(sessionPhaseToPresence(SessionPhase::Live) == SessionPresence::Live);
    CHECK(sessionPhaseToPresence(SessionPhase::Faltering) == SessionPresence::Faltering);
    CHECK(sessionPhaseToPresence(SessionPhase::Reconnecting) == SessionPresence::Linking);
    CHECK(sessionPhaseToPresence(SessionPhase::Stale) == SessionPresence::Stale);
    CHECK(sessionPhaseToPresence(SessionPhase::Failed) == SessionPresence::Stale);
    // Failed/Stale set the "Needs pairing" marker; nothing else does.
    CHECK(sessionPhaseIsStaleMarker(SessionPhase::Failed));
    CHECK(sessionPhaseIsStaleMarker(SessionPhase::Stale));
    CHECK_FALSE(sessionPhaseIsStaleMarker(SessionPhase::Live));
    CHECK_FALSE(sessionPhaseIsStaleMarker(SessionPhase::Reconnecting));
}

TEST_CASE("reduce is total over every phase and event", "[session-fsm]") {
    const std::vector<SessionEvent> events{
        se::Connect{ConnectIntent::UserInitiated, false},
        se::Connect{ConnectIntent::UserInitiated, true},
        se::Connect{ConnectIntent::AutoReconnect, false},
        se::RestClassified{RestVerdict::Ok},
        se::RestClassified{RestVerdict::Unauthorized},
        se::RestClassified{RestVerdict::VersionMismatch},
        se::RestClassified{RestVerdict::ShuttingDown},
        se::RestClassified{RestVerdict::Unreachable},
        se::RestClassified{RestVerdict::ServerError},
        se::PairClassified{PairVerdict::Success},
        se::PairClassified{PairVerdict::Pending},
        se::PairClassified{PairVerdict::AuthRequired},
        se::PairClassified{PairVerdict::VersionMismatch},
        se::PairClassified{PairVerdict::Unreachable},
        se::Linked{},
        se::HeartbeatMiss{0},
        se::HeartbeatMiss{kHeartbeatMissNotResponding},
        se::HeartbeatMiss{kHeartbeatMissDead},
        se::HeartbeatOk{},
        se::Closed{CloseAction::DropKeyRePair},
        se::Closed{CloseAction::StayDown},
        se::Closed{CloseAction::RetryBackoff},
        se::ReconcileDrift{},
        se::RetryTimerFired{},
        se::Disconnect{},
        se::Forget{},
    };
    const std::vector<SessionPhase> phases{SessionPhase::Discovered, SessionPhase::Pairing,
                                           SessionPhase::Linking,    SessionPhase::Live,
                                           SessionPhase::Faltering,  SessionPhase::Reconnecting,
                                           SessionPhase::Stale,      SessionPhase::Failed};
    for (const SessionPhase phase : phases) {
        const auto failure = (phase == SessionPhase::Failed)
                                 ? std::optional<SessionFailure>(SessionFailure::AuthRejected)
                                 : std::nullopt;
        for (const auto& e : events) {
            // The invariant: a surviving result in Failed always carries a typed
            // cause, and no phase outside Failed/Stale/Reconnecting retains one.
            const auto r = reduce(model(phase, ConnectIntent::UserInitiated, 1, 0, failure), e);
            if (r.next.has_value()) {
                if (r.next->phase == SessionPhase::Failed) {
                    CHECK(r.next->failure.has_value());
                } else if (r.next->phase != SessionPhase::Stale &&
                           r.next->phase != SessionPhase::Reconnecting) {
                    CHECK_FALSE(r.next->failure.has_value());
                }
            }
        }
    }
}

TEST_CASE("nextRetryAtMs is never stamped by reduce (clock-free)", "[session-fsm]") {
    // The reducer must never set an absolute wall time. It leaves nextRetryAtMs
    // at 0 and emits a ScheduleRetry the coordinator turns into now+delay.
    const std::vector<std::pair<SessionModel, SessionEvent>> retryCases{
        {model(SessionPhase::Linking), se::RestClassified{RestVerdict::Unreachable}},
        {model(SessionPhase::Live), se::HeartbeatMiss{kHeartbeatMissDead}},
        {model(SessionPhase::Live), se::Closed{CloseAction::RetryBackoff}},
        {model(SessionPhase::Pairing), se::PairClassified{PairVerdict::Unreachable}},
    };
    for (const auto& [m, e] : retryCases) {
        const auto r = reduce(m, e);
        REQUIRE(r.next.has_value());
        CHECK(r.next->phase == SessionPhase::Reconnecting);
        CHECK(r.next->nextRetryAtMs == 0);
        bool sawSchedule = false;
        for (const auto& fx : r.effects) {
            if (std::holds_alternative<sf::ScheduleRetry>(fx)) { sawSchedule = true; }
        }
        CHECK(sawSchedule);
    }
}

TEST_CASE("a successful Linked from any opening phase resets the backoff", "[session-fsm]") {
    for (const SessionPhase phase :
         {SessionPhase::Linking, SessionPhase::Reconnecting, SessionPhase::Pairing}) {
        const auto r =
            reduce(model(phase, ConnectIntent::AutoReconnect, /*retryAttempt=*/7), se::Linked{});
        REQUIRE(r.next.has_value());
        CHECK(r.next->phase == SessionPhase::Live);
        CHECK(r.next->retryAttempt == 0);
        CHECK_FALSE(r.next->failure.has_value());
    }
}
