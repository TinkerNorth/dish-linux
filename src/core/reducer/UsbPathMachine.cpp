// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The per-phase reducers, the unplug short-circuit, and the exact effect ordering
// each transition emits. The tests pin the effect lists in order, so a reordering
// is a behaviour change, not a cleanup.

#include "core/reducer/UsbPathMachine.h"

namespace dish::reducer {

namespace {

Reduction stay(UsbController c) { return Reduction{std::move(c), {}}; }

// A fresh Direct attempt: clear any stale failure, hold the framework, claim.
Reduction startClaim(UsbController c) {
    c.phase = UsbPhase::Claiming;
    c.failure.reset();
    return Reduction{std::move(c), {effect::ClearFailure{}, effect::BeginHold{}, effect::Claim{}}};
}

template <class T> const T* as(const UsbEvent& e) { return std::get_if<T>(&e); }

// The framework device came back: whichever placeholder was standing in for it goes, the slot
// binds to the real one, and Standard is persisted. `extra` is what the caller says about the
// claim that led here, which is the only thing the two callers differ on.
static Reduction bindReturnedFramework(const UsbController& c, int frameworkId,
                                       std::vector<UsbEffect> extra) {
    std::vector<UsbEffect> fx;
    if (c.syntheticId.has_value()) {
        fx.push_back(effect::RemoveSynthetic{*c.syntheticId});
    } else {
        fx.push_back(effect::EndHold{});
    }
    fx.push_back(effect::BindFramework{frameworkId});
    fx.push_back(effect::SetPref{PathChoice::Standard});
    for (auto& e : extra) { fx.push_back(std::move(e)); }

    UsbController n = c;
    n.phase = UsbPhase::Routed;
    n.frameworkId = frameworkId;
    n.syntheticId.reset();
    n.desired = PathChoice::Standard;
    n.failure.reset();
    return Reduction{std::move(n), std::move(fx)};
}

static Reduction markPermissionGranted(const UsbController& c) {
    UsbController n = c;
    n.hasPermission = true;
    return stay(std::move(n));
}

static Reduction adoptFrameworkId(const UsbController& c, int frameworkId) {
    UsbController n = c;
    n.frameworkId = frameworkId;
    return stay(std::move(n));
}

// A cable jiggle or claim aftermath; the slot waits for the device to return rather than
// settling, because the framework id it had is gone.
static Reduction routedFrameworkDown(const UsbController& c) {
    UsbController n = c;
    n.phase = UsbPhase::AwaitingFramework;
    n.frameworkId.reset();
    n.userInitiated = false;
    return Reduction{std::move(n), {effect::StartTimeout{}}};
}

// Standard is persisted as well, or the refused pick is silently re-attempted on every reconnect.
// A pick the user did not make fails quietly.
static Reduction routedPermissionDenied(const UsbController& c) {
    if (c.desired != PathChoice::Direct) { return stay(c); }
    UsbController n = c;
    n.desired = PathChoice::Standard;
    n.failure = DirectClaimFailure::PermissionDenied;
    std::vector<UsbEffect> fx;
    fx.push_back(effect::SetPref{PathChoice::Standard});
    fx.push_back(effect::MarkFailure{DirectClaimFailure::PermissionDenied});
    if (c.userInitiated) { fx.push_back(effect::Notify{UsbNotice::SwitchToDirectFailed}); }
    return Reduction{std::move(n), std::move(fx)};
}

// Permission is asked for only when the user picked Direct themselves: a prompt nobody asked for
// is worse than a pad that stays on Standard until they do.
static Reduction routedChoose(const UsbController& c, const event::Choose& ch) {
    if (ch.choice == PathChoice::Standard) {
        UsbController n = c;
        n.desired = PathChoice::Standard;
        return stay(std::move(n));
    }
    UsbController wanting = c;
    wanting.desired = PathChoice::Direct;
    wanting.userInitiated = ch.userInitiated;
    if (wanting.hasPermission) { return startClaim(std::move(wanting)); }
    if (ch.userInitiated) { return Reduction{std::move(wanting), {effect::RequestPermission{}}}; }
    return stay(std::move(wanting));
}

static Reduction routedPermissionGranted(const UsbController& c) {
    UsbController granted = c;
    granted.hasPermission = true;
    if (granted.desired == PathChoice::Direct) { return startClaim(std::move(granted)); }
    return stay(std::move(granted));
}

Reduction reduceRouted(const UsbController& c, const UsbEvent& event) {
    if (const auto* up = as<event::FrameworkUp>(event)) { return adoptFrameworkId(c, up->id); }
    if (as<event::FrameworkDown>(event) != nullptr) { return routedFrameworkDown(c); }
    if (as<event::PermissionGranted>(event) != nullptr) { return routedPermissionGranted(c); }
    if (as<event::PermissionDenied>(event) != nullptr) { return routedPermissionDenied(c); }
    if (const auto* ch = as<event::Choose>(event)) { return routedChoose(c, *ch); }
    return stay(c);
}

static Reduction claimingSucceeded(const UsbController& c, const event::ClaimSucceeded& ok) {
    UsbController n = c;
    n.phase = UsbPhase::Direct;
    n.syntheticId = ok.syntheticId;
    n.frameworkId.reset();
    n.failure.reset();
    return Reduction{std::move(n), {effect::EndHold{}, effect::ClearFailure{}}};
}

// The claim detached the kernel HID driver, so the framework device has to come back before
// anything settles on Standard.
static Reduction claimingLostTheFramework(const UsbController& c, const event::ClaimFailed& f) {
    UsbController n = c;
    n.phase = UsbPhase::AwaitingFramework;
    n.syntheticId.reset();
    n.failure = f.reason;
    return Reduction{std::move(n), {effect::StartTimeout{}}};
}

// Either the interface was never stolen (the framework slot is still live), or this model never
// re-enumerates as a framework gamepad, so there is nothing to wait for. Standard is persisted as
// well, or the failed pick is silently re-attempted on every reconnect.
static Reduction claimingSettledOnStandard(const UsbController& c, const event::ClaimFailed& f) {
    UsbController n = c;
    n.phase = UsbPhase::Routed;
    n.desired = PathChoice::Standard;
    n.syntheticId.reset();
    n.failure = f.reason;
    std::vector<UsbEffect> fx;
    fx.push_back(effect::EndHold{});
    fx.push_back(effect::SetPref{PathChoice::Standard});
    fx.push_back(effect::MarkFailure{f.reason});
    if (c.userInitiated) { fx.push_back(effect::Notify{UsbNotice::SwitchToDirectFailed}); }
    return Reduction{std::move(n), std::move(fx)};
}

Reduction reduceClaiming(const UsbController& c, const UsbEvent& event) {
    if (const auto* ok = as<event::ClaimSucceeded>(event)) { return claimingSucceeded(c, *ok); }
    if (const auto* f = as<event::ClaimFailed>(event)) {
        if (f->frameworkStolen && c.frameworkExpected) { return claimingLostTheFramework(c, *f); }
        return claimingSettledOnStandard(c, *f);
    }
    if (const auto* up = as<event::FrameworkUp>(event)) { return adoptFrameworkId(c, up->id); }
    if (as<event::PermissionGranted>(event) != nullptr) { return markPermissionGranted(c); }
    return stay(c);
}

// No framework gamepad follows this release, so there is nothing to wait for: the device-side
// restore in Release is the whole hand-back.
static Reduction releaseWithNoFrameworkToWaitFor(const UsbController& c, const event::Choose& ch) {
    UsbController n = c;
    n.phase = UsbPhase::Routed;
    n.desired = PathChoice::Standard;
    n.frameworkId.reset();
    n.syntheticId.reset();
    n.userInitiated = ch.userInitiated;
    n.failure.reset();
    std::vector<UsbEffect> fx;
    fx.push_back(effect::Release{});
    if (c.syntheticId.has_value()) { fx.push_back(effect::RemoveSynthetic{*c.syntheticId}); }
    fx.push_back(effect::SetPref{PathChoice::Standard});
    fx.push_back(effect::ClearFailure{});
    return Reduction{std::move(n), std::move(fx)};
}

// The synthetic stays as a held placeholder while the framework device comes back; if it never
// does, RestoreStuck lets the user choose.
static Reduction releaseAndAwaitFramework(const UsbController& c, const event::Choose& ch) {
    UsbController n = c;
    n.phase = UsbPhase::AwaitingFramework;
    n.userInitiated = ch.userInitiated;
    n.failure.reset();
    return Reduction{std::move(n), {effect::Release{}, effect::StartTimeout{}}};
}

static Reduction directChoose(const UsbController& c, const event::Choose& ch) {
    if (ch.choice != PathChoice::Standard) {
        UsbController n = c;
        n.desired = PathChoice::Direct;
        return stay(std::move(n));
    }
    if (!c.frameworkExpected) { return releaseWithNoFrameworkToWaitFor(c, ch); }
    return releaseAndAwaitFramework(c, ch);
}

Reduction reduceDirect(const UsbController& c, const UsbEvent& event) {
    if (const auto* ch = as<event::Choose>(event)) { return directChoose(c, *ch); }
    if (const auto* up = as<event::FrameworkUp>(event)) {
        UsbController n = c;
        n.frameworkId = up->id;
        return stay(std::move(n));
    }
    return stay(c);
}

// A wait that began with a failed claim shows why on the re-enumerated card; one that began with
// a deliberate release has nothing to report.
static Reduction awaitingFrameworkUp(const UsbController& c, const event::FrameworkUp& up) {
    std::vector<UsbEffect> extra;
    if (c.failure.has_value()) {
        extra.push_back(effect::MarkFailure{*c.failure});
        if (c.userInitiated) { extra.push_back(effect::Notify{UsbNotice::SwitchToDirectFailed}); }
    } else {
        extra.push_back(effect::ClearFailure{});
    }
    return bindReturnedFramework(c, up.id, std::move(extra));
}

// Never re-enumerated. Direct is NOT silently re-claimed under the user; the stuck state is
// surfaced with a live toggle instead.
static Reduction awaitingTimedOutWithSynthetic(const UsbController& c) {
    UsbController n = c;
    n.phase = UsbPhase::RestoreStuck;
    return Reduction{std::move(n),
                     {effect::MarkRestoreStuck{}, effect::Notify{UsbNotice::RestoreFailed}}};
}

// The device is gone from the OS. The reason is Dropped so the card asks for a physical replug
// rather than echoing the stale claim error.
static Reduction awaitingTimedOutWithNothing(const UsbController& c) {
    UsbController n = c;
    n.phase = UsbPhase::NeedsReplug;
    n.desired = PathChoice::Standard;
    n.failure = DirectClaimFailure::Dropped;
    return Reduction{std::move(n),
                     {effect::MarkNeedsReplug{}, effect::MarkFailure{DirectClaimFailure::Dropped},
                      effect::SetPref{PathChoice::Standard},
                      effect::Notify{UsbNotice::NeedsReplug}}};
}

Reduction reduceAwaiting(const UsbController& c, const UsbEvent& event) {
    if (const auto* up = as<event::FrameworkUp>(event)) { return awaitingFrameworkUp(c, *up); }
    if (as<event::Timeout>(event) != nullptr) {
        if (c.syntheticId.has_value()) { return awaitingTimedOutWithSynthetic(c); }
        return awaitingTimedOutWithNothing(c);
    }
    if (as<event::PermissionGranted>(event) != nullptr) { return markPermissionGranted(c); }
    return stay(c);
}

// Standard waits for the framework once more. That rarely succeeds without a replug, but from
// here it is the user's call.
static Reduction restoreStuckChoose(const UsbController& c, const event::Choose& ch) {
    UsbController n = c;
    n.userInitiated = ch.userInitiated;
    if (ch.choice == PathChoice::Direct) {
        n.desired = PathChoice::Direct;
        return Reduction{std::move(n), {effect::Reclaim{}}};
    }
    n.phase = UsbPhase::AwaitingFramework;
    return Reduction{std::move(n), {effect::ClearRestoreStuck{}, effect::StartTimeout{}}};
}

static Reduction restoreStuckClaimSucceeded(const UsbController& c,
                                            const event::ClaimSucceeded& ok) {
    UsbController n = c;
    n.phase = UsbPhase::Direct;
    n.syntheticId = ok.syntheticId;
    n.desired = PathChoice::Direct;
    n.failure.reset();
    return Reduction{std::move(n),
                     {effect::SetPref{PathChoice::Direct}, effect::ClearFailure{},
                      effect::Notify{UsbNotice::RolledBackToDirect}}};
}

// The device is gone. The Reclaim effector already dropped the synthetic placeholder, so only the
// Dropped reason needs surfacing.
static Reduction restoreStuckClaimFailed(const UsbController& c) {
    UsbController n = c;
    n.phase = UsbPhase::NeedsReplug;
    n.syntheticId.reset();
    n.failure = DirectClaimFailure::Dropped;
    return Reduction{std::move(n),
                     {effect::MarkFailure{DirectClaimFailure::Dropped},
                      effect::Notify{UsbNotice::RestoreFailed}}};
}

// The framework device came back after all, so the stuck mark goes with the placeholder.
static Reduction restoreStuckFrameworkUp(const UsbController& c, const event::FrameworkUp& up) {
    return bindReturnedFramework(c, up.id, {effect::ClearRestoreStuck{}, effect::ClearFailure{}});
}

Reduction reduceRestoreStuck(const UsbController& c, const UsbEvent& event) {
    if (const auto* ch = as<event::Choose>(event)) { return restoreStuckChoose(c, *ch); }
    if (const auto* ok = as<event::ClaimSucceeded>(event)) {
        return restoreStuckClaimSucceeded(c, *ok);
    }
    if (as<event::ClaimFailed>(event) != nullptr) { return restoreStuckClaimFailed(c); }
    if (const auto* up = as<event::FrameworkUp>(event)) { return restoreStuckFrameworkUp(c, *up); }
    if (as<event::PermissionGranted>(event) != nullptr) {
        UsbController n = c;
        n.hasPermission = true;
        return stay(std::move(n));
    }
    return stay(c);
}

Reduction reduceNeedsReplug(const UsbController& c, const UsbEvent& event) {
    if (const auto* up = as<event::FrameworkUp>(event)) {
        UsbController n = c;
        n.phase = UsbPhase::Routed;
        n.frameworkId = up->id;
        n.failure.reset();
        return Reduction{std::move(n), {effect::BindFramework{up->id}, effect::ClearFailure{}}};
    }
    if (const auto* ch = as<event::Choose>(event)) {
        UsbController n = c;
        n.desired = ch->choice;
        n.userInitiated = ch->userInitiated;
        return stay(std::move(n));
    }
    if (as<event::PermissionGranted>(event) != nullptr) {
        UsbController n = c;
        n.hasPermission = true;
        return stay(std::move(n));
    }
    return stay(c);
}

} // namespace

Reduction reduce(const UsbController& c, const UsbEvent& event) {
    // Physical unplug wins from any phase: tear down and forget.
    if (std::holds_alternative<event::UsbUnplugged>(event)) {
        std::vector<UsbEffect> fx;
        if (c.syntheticId.has_value()) { fx.push_back(effect::RemoveSynthetic{*c.syntheticId}); }
        if (c.phase == UsbPhase::Claiming || c.phase == UsbPhase::AwaitingFramework ||
            c.phase == UsbPhase::RestoreStuck || c.phase == UsbPhase::NeedsReplug) {
            fx.push_back(effect::EndHold{});
        }
        return Reduction{std::nullopt, std::move(fx)};
    }
    switch (c.phase) {
    case UsbPhase::Routed:
        return reduceRouted(c, event);
    case UsbPhase::Claiming:
        return reduceClaiming(c, event);
    case UsbPhase::Direct:
        return reduceDirect(c, event);
    case UsbPhase::AwaitingFramework:
        return reduceAwaiting(c, event);
    case UsbPhase::RestoreStuck:
        return reduceRestoreStuck(c, event);
    case UsbPhase::NeedsReplug:
        return reduceNeedsReplug(c, event);
    }
    return stay(c); // unreachable: the switch is total over UsbPhase
}

} // namespace dish::reducer
