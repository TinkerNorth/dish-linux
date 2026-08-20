// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The apply sequencer behind the wizard's Review page and Configure binding's
// Apply. One step per real async action, because the Connection step alone can
// sit for seconds while the kernel hands the device over and a bare spinner cannot say
// that. Connection is the USB path switch, skipped when the draft's path already
// matches; Destination is the REST round-trip that writes the binding.
//
// A Direct claim that times out is a fallback, not a failure: the pad still
// streams over Standard, so the run continues and the caller warns rather than
// erroring. Cancel is accepted only during Connection, where aborting a claim
// drops safely back to Standard; the REST round-trip cannot be half-applied.
//
// The wall clock lives in AppViewModel and arrives as Tick / *TimedOut events.

#pragma once

#include <optional>
#include <type_traits>
#include <variant>
#include <cstdint>

namespace dish::reducer {

enum class ApplyStep : std::uint8_t { Connection, Destination };
enum class ApplyStepState : std::uint8_t { Pending, Active, Done, Failed, Skipped };
enum class ApplyPhase : std::uint8_t { Idle, SwitchingPath, Binding, Succeeded, Failed, Cancelled };
enum class ApplyFailure : std::uint8_t {
    SlotGone,
    PathClaimTimeout,
    HostUnreachable,
    BindRejected,
    Cancelled
};

struct ApplyState {
    ApplyPhase phase = ApplyPhase::Idle;
    ApplyStepState connection = ApplyStepState::Pending;
    ApplyStepState destination = ApplyStepState::Pending;
    bool directFellBack = false; // a warning, not an error
    std::optional<ApplyFailure> failure;
    int elapsedMsOnStep = 0; // drives the 4s slow hint
    // Carried from Start so a settled path can tell a claim that did not land
    // from a deliberate return to Standard.
    bool wantsDirect = false;
};

namespace apply_event {
struct Start {
    bool needsPathSwitch = false;
    bool wantsDirect = false;
};
struct PathSettled {
    bool direct = false;
}; // pathPhase left `claiming`
struct PathTimedOut {}; // 20 s
struct BindAccepted {};
struct BindRejected {
    bool unreachable = false;
};
struct BindTimedOut {}; // 8 s
struct SlotVanished {};
struct Cancel {};
struct Tick {
    int deltaMs = 0;
};
} // namespace apply_event

using ApplyEvent =
    std::variant<apply_event::Start, apply_event::PathSettled, apply_event::PathTimedOut,
                 apply_event::BindAccepted, apply_event::BindRejected, apply_event::BindTimedOut,
                 apply_event::SlotVanished, apply_event::Cancel, apply_event::Tick>;

// True while the run is still moving (and therefore still cancel-/tick-able).
inline bool applyInFlight(const ApplyState& s) {
    return s.phase == ApplyPhase::SwitchingPath || s.phase == ApplyPhase::Binding;
}

// True iff the escape hatch is offered: the Connection step, and only it.
inline bool applyCancellable(const ApplyState& s) { return s.phase == ApplyPhase::SwitchingPath; }

inline ApplyState reduceApply(const ApplyState& s, const ApplyEvent& e) {
    ApplyState next = s;

    // Shared by the three ways Connection can end: settled, timed out, skipped.
    const auto enterBinding = [](ApplyState& st) {
        st.phase = ApplyPhase::Binding;
        st.destination = ApplyStepState::Active;
        st.elapsedMsOnStep = 0;
    };

    return std::visit(
        [&](auto&& ev) -> ApplyState {
            using E = std::decay_t<decltype(ev)>;

            if constexpr (std::is_same_v<E, apply_event::Start>) {
                // Start always restarts: a retry after a failure is the same
                // call, and the draft is still intact.
                next = ApplyState{};
                next.wantsDirect = ev.wantsDirect;
                if (ev.needsPathSwitch) {
                    next.phase = ApplyPhase::SwitchingPath;
                    next.connection = ApplyStepState::Active;
                } else {
                    next.connection = ApplyStepState::Skipped;
                    enterBinding(next);
                }
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::PathSettled>) {
                if (next.phase != ApplyPhase::SwitchingPath) { return next; }
                next.connection = ApplyStepState::Done;
                // Asked for Direct and got Standard: the claim lost. Warn rather
                // than fail, since the pad streams either way.
                next.directFellBack = next.wantsDirect && !ev.direct;
                enterBinding(next);
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::PathTimedOut>) {
                if (next.phase != ApplyPhase::SwitchingPath) { return next; }
                next.connection = ApplyStepState::Done;
                next.directFellBack = true;
                enterBinding(next);
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::BindAccepted>) {
                if (next.phase != ApplyPhase::Binding) { return next; }
                next.destination = ApplyStepState::Done;
                next.phase = ApplyPhase::Succeeded;
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::BindRejected>) {
                if (next.phase != ApplyPhase::Binding) { return next; }
                next.destination = ApplyStepState::Failed;
                next.phase = ApplyPhase::Failed;
                next.failure =
                    ev.unreachable ? ApplyFailure::HostUnreachable : ApplyFailure::BindRejected;
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::BindTimedOut>) {
                if (next.phase != ApplyPhase::Binding) { return next; }
                next.destination = ApplyStepState::Failed;
                next.phase = ApplyPhase::Failed;
                // A REST round-trip that never answers is an unreachable host.
                next.failure = ApplyFailure::HostUnreachable;
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::SlotVanished>) {
                // The pad went away mid-apply, as a Direct claim retiring the
                // framework slot id does. Terminal from any live phase; a run
                // that already finished keeps its result.
                if (next.phase == ApplyPhase::Succeeded || next.phase == ApplyPhase::Failed ||
                    next.phase == ApplyPhase::Cancelled) {
                    return next;
                }
                if (next.connection == ApplyStepState::Active) {
                    next.connection = ApplyStepState::Failed;
                }
                if (next.destination == ApplyStepState::Active) {
                    next.destination = ApplyStepState::Failed;
                }
                next.phase = ApplyPhase::Failed;
                next.failure = ApplyFailure::SlotGone;
                return next;
            } else if constexpr (std::is_same_v<E, apply_event::Cancel>) {
                if (next.phase != ApplyPhase::SwitchingPath) { return next; }
                next.connection = ApplyStepState::Failed;
                next.phase = ApplyPhase::Cancelled;
                next.failure = ApplyFailure::Cancelled;
                return next;
            } else {
                static_assert(std::is_same_v<E, apply_event::Tick>);
                if (!applyInFlight(next)) { return next; }
                next.elapsedMsOnStep += ev.deltaMs;
                return next;
            }
        },
        e);
}

} // namespace dish::reducer
