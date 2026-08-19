// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Per-satellite session lifecycle: a total (model, event) -> model + effects
// reducer. Effects are returned as data; the coordinator executes them.
// reduce() never reads a clock, so nextRetryAtMs is stamped by the coordinator.
// Verdicts arrive pre-classified (RestOutcome.h, CloseNotify.h); this file only
// decides what each verdict means for the lifecycle.

#pragma once

#include "core/reducer/Backoff.h"
#include "core/reducer/CloseNotify.h"
#include "core/reducer/RestOutcome.h"
#include "core/reducer/SatelliteLinkState.h"

#include <optional>
#include <variant>
#include <vector>

namespace dish::reducer {

// Duplicated from dish::net::SatelliteClient so this reducer stays Qt-free, and
// mirroring the satellite's HEARTBEAT_MISS_MAX. All three must stay in lockstep.
inline constexpr int kHeartbeatMissNotResponding = 2; // >= this -> Faltering
inline constexpr int kHeartbeatMissDead = 5;          // >= this -> Reconnecting (dead)

// Collapses onto SatelliteLinkState's SessionPresence via sessionPhaseToPresence.
enum class SessionPhase {
    Discovered,   // known/remembered, no live session
    Pairing,      // POST /api/pair in flight; no usable key yet
    Linking,      // session PUT in flight; key in hand
    Live,         // UDP tuple bound, heartbeat acks flowing
    Faltering,    // live but degraded: notResponding <= misses < dead
    Reconnecting, // parked on the backoff curve
    Stale,        // no session, but the pairing key is KEPT
    Failed,       // terminal, with a retained typed cause
};

// Populated iff phase == Failed, or phase == Stale with a recorded cause.
enum class SessionFailure {
    Unreachable,        // transport failure or a malformed reply; retryable
    VersionMismatch,    // 409 protocol skew; terminal
    AuthRejected,       // 401 NOT_PAIRED / BAD_PROOF, or no usable key; terminal
    Declined,           // reachable, but the satellite refused the pair; terminal
    ServerShuttingDown, // 503, kept distinct from Unreachable so the UI can say why
    ServerError,        // any other non-2xx; usually retryable
};

inline bool sessionFailureTerminal(SessionFailure f) {
    return f == SessionFailure::VersionMismatch || f == SessionFailure::AuthRejected ||
           f == SessionFailure::Declined;
}

// Gates whether a failure toasts. Every non-user origin is silent, so
// dish::net::ConnectIntent's RetryAfterDeath folds into AutoReconnect here.
enum class ConnectIntent {
    UserInitiated,
    AutoReconnect,
};

struct SessionModel {
    SessionPhase phase = SessionPhase::Discovered;
    std::optional<SessionFailure> failure;
    ConnectIntent intent = ConnectIntent::AutoReconnect;
    int retryAttempt = 0;        // 1-based; resets only on Linked or a user Connect
    long long nextRetryAtMs = 0; // absolute deadline; stamped by the coordinator, not reduce()
    int missedHeartbeats = 0;

    bool operator==(const SessionModel& o) const {
        return phase == o.phase && failure == o.failure && intent == o.intent &&
               retryAttempt == o.retryAttempt && nextRetryAtMs == o.nextRetryAtMs &&
               missedHeartbeats == o.missedHeartbeats;
    }
    bool operator!=(const SessionModel& o) const { return !(*this == o); }
};

namespace session_event {

struct Connect {
    ConnectIntent intent = ConnectIntent::UserInitiated;
    // True when no usable pairing key is held yet, so POST /api/pair must run first.
    bool needsPair = false;
    bool operator==(const Connect& o) const {
        return intent == o.intent && needsPair == o.needsPair;
    }
    bool operator!=(const Connect& o) const { return !(*this == o); }
};

// The PUT /api/connections reply, already run through classifyRest.
struct RestClassified {
    RestVerdict verdict = RestVerdict::Unreachable;
    bool operator==(const RestClassified& o) const { return verdict == o.verdict; }
    bool operator!=(const RestClassified& o) const { return !(*this == o); }
};

// The POST /api/pair reply, already run through classifyPair.
struct PairClassified {
    PairVerdict verdict = PairVerdict::Unreachable;
    bool operator==(const PairClassified& o) const { return verdict == o.verdict; }
    bool operator!=(const PairClassified& o) const { return !(*this == o); }
};

// Session PUT accepted AND the UDP socket opened. The only path to Live.
struct Linked {
    bool operator==(const Linked&) const { return true; }
    bool operator!=(const Linked&) const { return false; }
};

// `count` consecutive missed heartbeat acks, from one alive-poll tick.
struct HeartbeatMiss {
    int count = 0;
    bool operator==(const HeartbeatMiss& o) const { return count == o.count; }
    bool operator!=(const HeartbeatMiss& o) const { return !(*this == o); }
};

struct HeartbeatOk {
    bool operator==(const HeartbeatOk&) const { return true; }
    bool operator!=(const HeartbeatOk&) const { return false; }
};

// An authenticated close-notify, already mapped by closeActionForReason. The raw
// reason byte is never re-interpreted here.
struct Closed {
    CloseAction action = CloseAction::RetryBackoff;
    bool operator==(const Closed& o) const { return action == o.action; }
    bool operator!=(const Closed& o) const { return !(*this == o); }
};

// Reconcile.h decided the applied topology drifted from desired. The diff itself
// stays in the coordinator; this only says "converge".
struct ReconcileDrift {
    bool operator==(const ReconcileDrift&) const { return true; }
    bool operator!=(const ReconcileDrift&) const { return false; }
};

struct RetryTimerFired {
    bool operator==(const RetryTimerFired&) const { return true; }
    bool operator!=(const RetryTimerFired&) const { return false; }
};

// Graceful. Lands in Stale, not Failed: the pairing key is kept so one Connect re-links.
struct Disconnect {
    bool operator==(const Disconnect&) const { return true; }
    bool operator!=(const Disconnect&) const { return false; }
};

struct Forget {
    bool operator==(const Forget&) const { return true; }
    bool operator!=(const Forget&) const { return false; }
};

} // namespace session_event

using SessionEvent =
    std::variant<session_event::Connect, session_event::RestClassified,
                 session_event::PairClassified, session_event::Linked, session_event::HeartbeatMiss,
                 session_event::HeartbeatOk, session_event::Closed, session_event::ReconcileDrift,
                 session_event::RetryTimerFired, session_event::Disconnect, session_event::Forget>;

namespace session_effect {

// PUT /api/connections: the declarative session open (identity + proof + full
// topology). Reply comes back as RestClassified, a bound socket as Linked.
struct OpenSession {
    bool operator==(const OpenSession&) const { return true; }
    bool operator!=(const OpenSession&) const { return false; }
};

// POST /api/pair; reply comes back as PairClassified.
struct Pair {
    bool operator==(const Pair&) const { return true; }
    bool operator!=(const Pair&) const { return false; }
};

// Bind the session tuple from a successful session PUT; Linked comes back.
struct BindUdp {
    bool operator==(const BindUdp&) const { return true; }
    bool operator!=(const BindUdp&) const { return false; }
};

// A relative delay, not an absolute time, so reduce() stays clock-free. The
// coordinator stamps nextRetryAtMs = now + delayMs when it arms the timer.
struct ScheduleRetry {
    int delayMs = 0;
    bool operator==(const ScheduleRetry& o) const { return delayMs == o.delayMs; }
    bool operator!=(const ScheduleRetry& o) const { return !(*this == o); }
};

struct DropKey {
    bool operator==(const DropKey&) const { return true; }
    bool operator!=(const DropKey&) const { return false; }
};

// `loud` (a toast) only for a user-initiated attempt; otherwise the row chip is the cue.
struct Notify {
    SessionFailure reason = SessionFailure::Unreachable;
    bool loud = false;
    bool operator==(const Notify& o) const { return reason == o.reason && loud == o.loud; }
    bool operator!=(const Notify& o) const { return !(*this == o); }
};

struct ClearFailure {
    bool operator==(const ClearFailure&) const { return true; }
    bool operator!=(const ClearFailure&) const { return false; }
};

struct StartHeartbeat {
    bool operator==(const StartHeartbeat&) const { return true; }
    bool operator!=(const StartHeartbeat&) const { return false; }
};

struct StopHeartbeat {
    bool operator==(const StopHeartbeat&) const { return true; }
    bool operator!=(const StopHeartbeat&) const { return false; }
};

} // namespace session_effect

using SessionEffect =
    std::variant<session_effect::OpenSession, session_effect::Pair, session_effect::BindUdp,
                 session_effect::ScheduleRetry, session_effect::DropKey, session_effect::Notify,
                 session_effect::ClearFailure, session_effect::StartHeartbeat,
                 session_effect::StopHeartbeat>;

// next == nullopt means "remove this session from tracking".
// Do not rename to `Reduction`: UsbPathMachine.h declares its own in this same
// namespace, and a TU that includes both would hit an ODR redefinition.
struct SessionReduction {
    std::optional<SessionModel> next;
    std::vector<SessionEffect> effects;
};

// A pure predicate so the coordinator and the UI collapse phases identically.
inline SessionPresence sessionPhaseToPresence(SessionPhase phase) {
    switch (phase) {
    case SessionPhase::Live:
        return SessionPresence::Live;
    case SessionPhase::Faltering:
        return SessionPresence::Faltering;
    case SessionPhase::Pairing:
    case SessionPhase::Linking:
    case SessionPhase::Reconnecting:
        // A silent backoff retry reads "Connecting…", not a distinct state.
        return SessionPresence::Linking;
    case SessionPhase::Stale:
    case SessionPhase::Failed:
        return SessionPresence::Stale;
    case SessionPhase::Discovered:
    default:
        return SessionPresence::Idle;
    }
}

// The isStale argument to satelliteLinkState.
inline bool sessionPhaseIsStaleMarker(SessionPhase phase) {
    return phase == SessionPhase::Stale || phase == SessionPhase::Failed;
}

// Total: any (phase x event) not handled below returns the model unchanged.
inline SessionReduction reduce(const SessionModel& s, const SessionEvent& event) {
    using namespace session_event;
    namespace fx = session_effect;

    const bool live = s.phase == SessionPhase::Live || s.phase == SessionPhase::Faltering;
    const bool loud = s.intent == ConnectIntent::UserInitiated;

    auto toReconnecting = [&](SessionFailure cause, bool emitNotify) -> SessionReduction {
        SessionModel next = s;
        next.phase = SessionPhase::Reconnecting;
        next.retryAttempt = s.retryAttempt + 1;
        next.failure = cause;
        next.nextRetryAtMs = 0;
        const int delay = static_cast<int>(backoffDelayMs(next.retryAttempt));
        SessionReduction r;
        r.next = next;
        if (live) { r.effects.push_back(fx::StopHeartbeat{}); }
        r.effects.push_back(fx::ScheduleRetry{delay});
        if (emitNotify) { r.effects.push_back(fx::Notify{cause, loud}); }
        return r;
    };

    auto toFailed = [&](SessionFailure cause, bool dropKey) -> SessionReduction {
        SessionModel next = s;
        next.phase = SessionPhase::Failed;
        next.failure = cause;
        next.nextRetryAtMs = 0;
        SessionReduction r;
        r.next = next;
        if (live) { r.effects.push_back(fx::StopHeartbeat{}); }
        if (dropKey) { r.effects.push_back(fx::DropKey{}); }
        r.effects.push_back(fx::Notify{cause, loud});
        return r;
    };

    return std::visit(
        [&](const auto& e) -> SessionReduction {
            using E = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<E, Connect>) {
                if (live) { return SessionReduction{s, {}}; }
                SessionModel next = s;
                next.intent = e.intent;
                next.failure = std::nullopt;
                next.nextRetryAtMs = 0;
                next.missedHeartbeats = 0;
                // A user tap restarts the backoff curve; an auto-reconnect continues it.
                if (e.intent == ConnectIntent::UserInitiated) { next.retryAttempt = 0; }
                SessionReduction r;
                if (e.needsPair) {
                    next.phase = SessionPhase::Pairing;
                    r.next = next;
                    r.effects = {fx::ClearFailure{}, fx::Pair{}};
                } else {
                    next.phase = SessionPhase::Linking;
                    r.next = next;
                    r.effects = {fx::ClearFailure{}, fx::OpenSession{}};
                }
                return r;
            }

            else if constexpr (std::is_same_v<E, PairClassified>) {
                if (s.phase != SessionPhase::Pairing) { return SessionReduction{s, {}}; }
                switch (e.verdict) {
                case PairVerdict::Success: {
                    SessionModel next = s;
                    next.phase = SessionPhase::Linking;
                    return SessionReduction{next, {fx::OpenSession{}}};
                }
                case PairVerdict::Pending:
                    // A staged grant; the approval poll runs outside this machine.
                    return SessionReduction{s, {}};
                case PairVerdict::AuthRequired:
                    // Reachable, but no usable key adopted: the pair was refused.
                    return toFailed(SessionFailure::Declined, /*dropKey=*/false);
                case PairVerdict::VersionMismatch:
                    return toFailed(SessionFailure::VersionMismatch, /*dropKey=*/false);
                case PairVerdict::Unreachable:
                    return toReconnecting(SessionFailure::Unreachable, /*emitNotify=*/true);
                }
                return SessionReduction{s, {}};
            }

            else if constexpr (std::is_same_v<E, RestClassified>) {
                if (s.phase != SessionPhase::Linking && s.phase != SessionPhase::Reconnecting) {
                    return SessionReduction{s, {}}; // stale reply for a settled attempt
                }
                switch (e.verdict) {
                case RestVerdict::Ok:
                    // Not yet live: the UDP socket must still open and send Linked.
                    return SessionReduction{s, {}};
                case RestVerdict::Unauthorized:
                    return toFailed(SessionFailure::AuthRejected, /*dropKey=*/true);
                case RestVerdict::VersionMismatch:
                    return toFailed(SessionFailure::VersionMismatch, /*dropKey=*/false);
                case RestVerdict::ShuttingDown:
                    return toReconnecting(SessionFailure::ServerShuttingDown, /*emitNotify=*/true);
                case RestVerdict::ServerError:
                    return toReconnecting(SessionFailure::ServerError, /*emitNotify=*/true);
                case RestVerdict::Unreachable:
                    return toReconnecting(SessionFailure::Unreachable, /*emitNotify=*/true);
                }
                return SessionReduction{s, {}};
            }

            else if constexpr (std::is_same_v<E, Linked>) {
                if (s.phase == SessionPhase::Discovered || s.phase == SessionPhase::Stale ||
                    s.phase == SessionPhase::Failed) {
                    return SessionReduction{s, {}}; // stray confirmation
                }
                SessionModel next = s;
                next.phase = SessionPhase::Live;
                next.failure = std::nullopt;
                next.retryAttempt = 0; // the only place success resets the backoff curve
                next.nextRetryAtMs = 0;
                next.missedHeartbeats = 0;
                return SessionReduction{next, {fx::ClearFailure{}, fx::StartHeartbeat{}}};
            }

            else if constexpr (std::is_same_v<E, HeartbeatMiss>) {
                if (!live) { return SessionReduction{s, {}}; }
                SessionModel next = s;
                next.missedHeartbeats = e.count;
                if (e.count >= kHeartbeatMissDead) {
                    // Always a silent retry: no user tap sits behind a heartbeat death.
                    next.phase = SessionPhase::Reconnecting;
                    next.retryAttempt = s.retryAttempt + 1;
                    next.failure = SessionFailure::Unreachable;
                    next.nextRetryAtMs = 0;
                    const int delay = static_cast<int>(backoffDelayMs(next.retryAttempt));
                    return SessionReduction{next, {fx::StopHeartbeat{}, fx::ScheduleRetry{delay}}};
                }
                if (e.count >= kHeartbeatMissNotResponding) {
                    // Degraded but still up: the socket stays open, so no effects.
                    next.phase = SessionPhase::Faltering;
                    return SessionReduction{next, {}};
                }
                return SessionReduction{next, {}};
            }

            else if constexpr (std::is_same_v<E, HeartbeatOk>) {
                if (!live) { return SessionReduction{s, {}}; }
                SessionModel next = s;
                next.missedHeartbeats = 0;
                if (s.phase == SessionPhase::Faltering) { next.phase = SessionPhase::Live; }
                return SessionReduction{next, {}};
            }

            else if constexpr (std::is_same_v<E, Closed>) {
                if (!live) { return SessionReduction{s, {}}; }
                switch (e.action) {
                case CloseAction::DropKeyRePair: {
                    // Trust revoked. Stale-with-cause rather than Failed: the row
                    // stays, reading "Needs pairing".
                    SessionModel next = s;
                    next.phase = SessionPhase::Stale;
                    next.failure = SessionFailure::AuthRejected;
                    next.retryAttempt = 0;
                    next.nextRetryAtMs = 0;
                    next.missedHeartbeats = 0;
                    return SessionReduction{next, {fx::StopHeartbeat{}, fx::DropKey{}}};
                }
                case CloseAction::StayDown: {
                    // A newer PUT owns the session; key kept, nothing more to do.
                    SessionModel next = s;
                    next.phase = SessionPhase::Stale;
                    next.failure = std::nullopt;
                    next.retryAttempt = 0;
                    next.nextRetryAtMs = 0;
                    next.missedHeartbeats = 0;
                    return SessionReduction{next, {fx::StopHeartbeat{}}};
                }
                case CloseAction::RetryBackoff:
                    return toReconnecting(SessionFailure::Unreachable, /*emitNotify=*/false);
                }
                return SessionReduction{s, {}};
            }

            else if constexpr (std::is_same_v<E, ReconcileDrift>) {
                if (!live) { return SessionReduction{s, {}}; }
                SessionModel next = s;
                next.phase = SessionPhase::Linking;
                next.missedHeartbeats = 0;
                // Not a failure, so retryAttempt is preserved.
                next.failure = std::nullopt;
                return SessionReduction{next, {fx::StopHeartbeat{}, fx::OpenSession{}}};
            }

            else if constexpr (std::is_same_v<E, RetryTimerFired>) {
                if (s.phase != SessionPhase::Reconnecting) { return SessionReduction{s, {}}; }
                SessionModel next = s;
                next.phase = SessionPhase::Linking;
                next.nextRetryAtMs = 0; // deadline consumed; retryAttempt keeps running
                return SessionReduction{next, {fx::OpenSession{}}};
            }

            else if constexpr (std::is_same_v<E, Disconnect>) {
                SessionModel next = s;
                next.phase = SessionPhase::Stale;
                next.failure = std::nullopt;
                next.retryAttempt = 0;
                next.nextRetryAtMs = 0;
                next.missedHeartbeats = 0;
                SessionReduction r;
                r.next = next;
                if (live) { r.effects.push_back(fx::StopHeartbeat{}); }
                return r;
            }

            else if constexpr (std::is_same_v<E, Forget>) {
                SessionReduction r;
                r.next = std::nullopt;
                if (live) { r.effects.push_back(fx::StopHeartbeat{}); }
                r.effects.push_back(fx::DropKey{});
                return r;
            }

            else {
                return SessionReduction{s, {}};
            }
        },
        event);
}

} // namespace dish::reducer
