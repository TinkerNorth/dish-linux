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

namespace dish::reducer {

enum class PairPhase {
    Idle,
    Submitting, // a classified Success stays here until SessionConfirmedLive
    Succeeded,
    Failed, // retryable via a fresh Submit
};

enum class PairFailure {
    WrongPin,        // reachable and parsed, but no usable key adopted
    VersionMismatch, // 409 protocol skew
    Unreachable,     // transport failure or an empty body
    // No PairVerdict arm maps here yet. Carried so the forward and reverse
    // pairing vocabularies match.
    Declined,
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
inline PairingState reducePairing(const PairingState& s, const PairEvent& event) {
    return std::visit(
        [&](const auto& e) -> PairingState {
            using E = std::decay_t<decltype(e)>;

            // ── Submit: start or restart an attempt from any phase ──────────
            if constexpr (std::is_same_v<E, pair_event::Submit>) {
                PairingState next;
                next.phase = PairPhase::Submitting;
                next.failure = std::nullopt; // a new attempt clears any prior reason
                next.pin = e.pin;            // carry the PIN while submitting
                return next;
            }

            // ── ReplyClassified: map the verdict, only while Submitting ─────
            else if constexpr (std::is_same_v<E, pair_event::ReplyClassified>) {
                if (s.phase != PairPhase::Submitting) {
                    return s; // late/stale reply for a settled attempt — ignore
                }
                switch (e.verdict) {
                case PairVerdict::Success:
                    // Key adopted, session opening — NOT done yet. Stay put and
                    // wait for SessionConfirmedLive to confirm the live session.
                    return s;
                case PairVerdict::Pending:
                    // Forward Path A doesn't expect Pending; it is not a terminal
                    // failure here. Keep Submitting (the manager resolves it).
                    return s;
                case PairVerdict::AuthRequired: {
                    // Reachable but no usable key adopted: the PIN was rejected.
                    PairingState next = s;
                    next.phase = PairPhase::Failed;
                    next.failure = PairFailure::WrongPin;
                    return next; // pin retained for the UI / a retry
                }
                case PairVerdict::VersionMismatch: {
                    PairingState next = s;
                    next.phase = PairPhase::Failed;
                    next.failure = PairFailure::VersionMismatch;
                    return next;
                }
                case PairVerdict::Unreachable: {
                    PairingState next = s;
                    next.phase = PairPhase::Failed;
                    next.failure = PairFailure::Unreachable;
                    return next;
                }
                }
                // Defensive: a bogus verdict cast lands here. The switch is
                // exhaustive over the enum, so this is unreachable in practice;
                // treat an unknown verdict as "keep waiting" rather than throw.
                return s;
            }

            // ── SessionConfirmedLive: the ONLY path to Succeeded ────────────
            else if constexpr (std::is_same_v<E, pair_event::SessionConfirmedLive>) {
                if (s.phase != PairPhase::Submitting) {
                    return s; // no attempt is awaiting confirmation — ignore
                }
                PairingState next;
                next.phase = PairPhase::Succeeded;
                next.failure = std::nullopt;
                next.pin.clear(); // attempt is done; don't retain the PIN
                return next;
            }

            // ── Cancel: unconditional return to Idle from any phase ─────────
            else if constexpr (std::is_same_v<E, pair_event::Cancel>) {
                return PairingState{}; // fresh Idle state (no failure, no pin)
            }

            // ── Total fallback (no event type reaches here) ─────────────────
            else {
                return s;
            }
        },
        event);
}

} // namespace dish::reducer
