// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Forward pairing (the user types a PIN) as a total (state, event) -> state
// reducer. Verdicts arrive pre-classified from classifyPair; this file only
// decides what each one means for the lifecycle. Succeeded is reached ONLY on an
// explicit SessionConfirmedLive, never inferred from an adopted key, so success
// is reported when the session is live rather than a beat early.

#pragma once

#include "core/reducer/RestOutcome.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <cstdint>

namespace dish::reducer {

enum class PairPhase : std::uint8_t {
    Idle,
    Submitting, // a classified Success stays here until SessionConfirmedLive
    Succeeded,
    Failed, // retryable via a fresh Submit
};

enum class PairFailure : std::uint8_t {
    WrongPin,        // reachable and parsed, but no usable key adopted
    VersionMismatch, // 409 protocol skew
    Unreachable,     // transport failure or an empty body
    // No PairVerdict arm maps here yet. Carried so the forward and reverse
    // pairing vocabularies match.
    Declined,
    // The pinned certificate changed. Terminal, and not a PIN problem: only an
    // explicit Forget can clear the pin, so a retry cannot succeed.
    IdentityChanged,
};

struct PairingState {
    PairPhase phase = PairPhase::Idle;
    std::optional<PairFailure> failure; // set iff phase == Failed
    std::string pin;                    // retained on Failed so a retry can show it

    bool operator==(const PairingState& o) const {
        return phase == o.phase && failure == o.failure && pin == o.pin;
    }
    bool operator!=(const PairingState& o) const { return !(*this == o); }
};

// ── Events ────────────────────────────────────────────────────────────────────

namespace pair_event {

// The user submitted a PIN. Starts (or restarts) a pairing attempt.
struct Submit {
    std::string pin;
    bool operator==(const Submit& o) const { return pin == o.pin; }
    bool operator!=(const Submit& o) const { return !(*this == o); }
};

// The network reply to the POST /api/pair, ALREADY classified by the existing
// pure classifier (classifyPair in RestOutcome.h). The reducer maps the verdict
// to the next phase — it does not re-classify.
struct ReplyClassified {
    PairVerdict verdict = PairVerdict::Unreachable;
    bool operator==(const ReplyClassified& o) const { return verdict == o.verdict; }
    bool operator!=(const ReplyClassified& o) const { return !(*this == o); }
};

// The session actually reached Connected. The only event that drives Succeeded.
struct SessionConfirmedLive {
    bool operator==(const SessionConfirmedLive&) const { return true; }
    bool operator!=(const SessionConfirmedLive&) const { return false; }
};

// The user (or a superseding request) cancelled. Returns to Idle from any phase.
struct Cancel {
    bool operator==(const Cancel&) const { return true; }
    bool operator!=(const Cancel&) const { return false; }
};

} // namespace pair_event

using PairEvent = std::variant<pair_event::Submit, pair_event::ReplyClassified,
                               pair_event::SessionConfirmedLive, pair_event::Cancel>;

// ── Reducer ─────────────────────────────────────────────────────────────────
// The total forward-pairing reducer: (state, event) -> next state. Defined for
// EVERY (phase x event); never throws. The rules, by event:
//
//   Submit{pin}
//     Idle / Failed / Succeeded -> Submitting(pin), failure cleared.
//       From Failed this is a RETRY: the prior failure is dropped so the UI
//       leaves the error state. From Succeeded it is a fresh attempt (e.g. the
//       session dropped and the user re-pairs). The submitted pin is carried.
//     Submitting -> Submitting(pin): a re-submit while one is already in flight
//       just adopts the newest pin (the manager single-flights the wire).
//
//   ReplyClassified{verdict}  — only meaningful while Submitting:
//     Success         -> stay Submitting (key adopted, session opening; we wait
//                        for SessionConfirmedLive before reporting Succeeded).
//     Pending         -> stay Submitting. On a FORWARD submit a Pending is NOT a
//                        terminal failure: Path A doesn't expect it, but the
//                        manager may still resolve it (or it degrades), so we
//                        keep waiting rather than flipping to Failed.
//     AuthRequired    -> Failed(WrongPin).
//     VersionMismatch -> Failed(VersionMismatch).
//     Unreachable     -> Failed(Unreachable).
//     A ReplyClassified that arrives in any non-Submitting phase is a late /
//     stale reply for a settled attempt and is IGNORED (state unchanged).
//
//   SessionConfirmedLive
//     Submitting -> Succeeded (pin + failure cleared). This is the ONLY path to
//       Succeeded. In any other phase it is a stray confirmation (no attempt is
//       waiting on it) and is IGNORED.
//
//   Cancel
//     Any phase -> Idle (a fresh, empty state). Total and unconditional.
//
// Anything not named above is a no-op for that phase (returns the state
// unchanged), making every combination explicit.

// Start or restart an attempt, from any phase. A new attempt clears any prior reason and carries
// the PIN for as long as it is submitting.
inline PairingState onPairSubmit(const pair_event::Submit& e) {
    PairingState next;
    next.phase = PairPhase::Submitting;
    next.failure = std::nullopt;
    next.pin = e.pin;
    return next;
}

// The PIN is retained, because the UI shows it back and a retry reuses it.
inline PairingState pairFailure(const PairingState& s, const PairFailure failure) {
    PairingState next = s;
    next.phase = PairPhase::Failed;
    next.failure = failure;
    return next;
}

// A verdict only moves a Submitting attempt; anything later is a stale reply for one that has
// already settled. Success and Pending both keep waiting: a key being adopted is not the same as
// the session being live, and only SessionConfirmedLive says that.
inline PairingState onPairReply(const PairingState& s, const pair_event::ReplyClassified& e) {
    if (s.phase != PairPhase::Submitting) { return s; }
    switch (e.verdict) {
    case PairVerdict::Success:
    case PairVerdict::Pending:
        return s;
    case PairVerdict::AuthRequired:
        // Reachable but no usable key adopted: the PIN was rejected.
        return pairFailure(s, PairFailure::WrongPin);
    case PairVerdict::VersionMismatch:
        return pairFailure(s, PairFailure::VersionMismatch);
    case PairVerdict::Unreachable:
        return pairFailure(s, PairFailure::Unreachable);
    case PairVerdict::IdentityChanged:
        return pairFailure(s, PairFailure::IdentityChanged);
    }
    // A bogus verdict cast lands here. The switch is exhaustive over the enum, so this is
    // unreachable in practice; an unknown verdict keeps waiting rather than throws.
    return s;
}

// The ONLY path to Succeeded, and only from an attempt that is awaiting confirmation. The attempt
// is done, so the PIN is not retained.
inline PairingState onSessionConfirmedLive(const PairingState& s) {
    if (s.phase != PairPhase::Submitting) { return s; }
    PairingState next;
    next.phase = PairPhase::Succeeded;
    next.failure = std::nullopt;
    next.pin.clear();
    return next;
}

inline PairingState reducePairing(const PairingState& s, const PairEvent& event) {
    return std::visit(
        [&](const auto& e) -> PairingState {
            using E = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<E, pair_event::Submit>) {
                return onPairSubmit(e);
            } else if constexpr (std::is_same_v<E, pair_event::ReplyClassified>) {
                return onPairReply(s, e);
            } else if constexpr (std::is_same_v<E, pair_event::SessionConfirmedLive>) {
                return onSessionConfirmedLive(s);
            } else if constexpr (std::is_same_v<E, pair_event::Cancel>) {
                // Unconditional return to a fresh Idle: no failure, no pin.
                return PairingState{};
            } else {
                return s;
            }
        },
        event);
}

} // namespace dish::reducer
