// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The per-controller USB input-path lifecycle FSM. Every (phase x event) pair is
// total and never throws, so an odd transition cannot silently drop a slot the
// way scattered imperative logic could. Effects come back as data; `reduce` does
// no IO. The coordinator (source/usb/UsbGamepadManager) turns world changes into
// events, runs `reduce`, and executes the effects against the real subsystems.
//
// "Framework" here is SDL and "Direct" is the raw-HID claim. xpad binds
// Xbox-class pads from raw HID, so Direct mainly serves DualSense/DS4/8BitDo.

#pragma once

#include "core/reducer/DirectClaimFailure.h"
#include "core/reducer/PathChoice.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dish::reducer {

enum class UsbPhase {
    Routed,            // Standard: a framework device is present
    Claiming,          // a direct-mode claim is in flight
    Direct,            // claimed and streaming
    AwaitingFramework, // released or claim-failed; waiting on re-enumeration
    RestoreStuck,      // a return to Standard never re-enumerated; the user picks
                       // Direct, retry, or replug
    NeedsReplug,       // present, but the OS never gave the device back
};

// Keyed by vpKey (vendorId<<16 | productId) in the coordinator.
struct UsbController {
    int vendorId = 0;
    int productId = 0;
    std::string name;
    UsbPhase phase = UsbPhase::Routed;
    bool usbPresent = true;
    std::optional<int> frameworkId;
    std::optional<int> syntheticId;
    bool hasPermission = false;
    PathChoice desired = PathChoice::Standard;
    bool userInitiated = false; // gates user notices
    // Carried with the controller across path switches.
    std::optional<std::string> connId;
    std::optional<int> type;
    // Remembered between the failure and the Standard re-settle, so the
    // re-enumerated framework card can still show the cause.
    std::optional<DirectClaimFailure> failure;
    // False for models whose stand-alone identity is not a gamepad (the Steam
    // Controller emulates a keyboard and mouse): a release then has no
    // framework device to wait for, so the FSM settles straight back to Routed
    // instead of timing out into a false RestoreStuck.
    bool frameworkExpected = true;

    bool operator==(const UsbController& o) const {
        return vendorId == o.vendorId && productId == o.productId && name == o.name &&
               phase == o.phase && usbPresent == o.usbPresent && frameworkId == o.frameworkId &&
               syntheticId == o.syntheticId && hasPermission == o.hasPermission &&
               desired == o.desired && userInitiated == o.userInitiated && connId == o.connId &&
               type == o.type && failure == o.failure && frameworkExpected == o.frameworkExpected;
    }
    bool operator!=(const UsbController& o) const { return !(*this == o); }
};

// ── Events ────────────────────────────────────────────────────────────────

namespace event {

struct FrameworkUp {
    int id = 0;
    bool operator==(const FrameworkUp& o) const { return id == o.id; }
};
struct FrameworkDown {
    bool operator==(const FrameworkDown&) const { return true; }
};
struct UsbUnplugged {
    bool operator==(const UsbUnplugged&) const { return true; }
};
struct PermissionGranted {
    bool operator==(const PermissionGranted&) const { return true; }
};
struct PermissionDenied {
    bool operator==(const PermissionDenied&) const { return true; }
};
struct Choose {
    PathChoice choice = PathChoice::Standard;
    bool userInitiated = false;
    bool operator==(const Choose& o) const {
        return choice == o.choice && userInitiated == o.userInitiated;
    }
};
struct ClaimSucceeded {
    int syntheticId = 0;
    bool operator==(const ClaimSucceeded& o) const { return syntheticId == o.syntheticId; }
};
// `frameworkStolen` means the kernel HID driver was already detached when the
// claim failed, so the framework device must re-enumerate before Standard can
// settle. When false the framework was never touched and the slot is usable now.
struct ClaimFailed {
    DirectClaimFailure reason = DirectClaimFailure::Busy;
    bool frameworkStolen = false;
    bool operator==(const ClaimFailed& o) const {
        return reason == o.reason && frameworkStolen == o.frameworkStolen;
    }
};
struct Timeout {
    bool operator==(const Timeout&) const { return true; }
};

} // namespace event

using UsbEvent = std::variant<event::FrameworkUp, event::FrameworkDown, event::UsbUnplugged,
                              event::PermissionGranted, event::PermissionDenied, event::Choose,
                              event::ClaimSucceeded, event::ClaimFailed, event::Timeout>;

// The coordinator maps these to localized strings.
enum class UsbNotice {
    SwitchToDirectFailed,
    NeedsReplug,
    RolledBackToDirect,
    RestoreFailed,
};

// ── Effects (returned as data; executed by the coordinator) ─────────────────

namespace effect {

// Open, claim the interface, attach the read loop, register the synthetic and
// bind. The coordinator feeds the outcome back as ClaimSucceeded/ClaimFailed.
struct Claim {
    bool operator==(const Claim&) const { return true; }
};
// Rollback to a known-good Direct claim, dropping the synthetic placeholder.
// Only emitted when the user picks Direct out of RestoreStuck.
struct Reclaim {
    bool operator==(const Reclaim&) const { return true; }
};
// Detach the read loop and release the interface, keeping the synthetic entry as
// a held placeholder.
struct Release {
    bool operator==(const Release&) const { return true; }
};
struct RequestPermission {
    bool operator==(const RequestPermission&) const { return true; }
};
struct BindFramework {
    int frameworkId = 0;
    bool operator==(const BindFramework& o) const { return frameworkId == o.frameworkId; }
};
struct RemoveSynthetic {
    int syntheticId = 0;
    bool operator==(const RemoveSynthetic& o) const { return syntheticId == o.syntheticId; }
};
// Hold the framework device through the transition, suppressing the grace reaper.
struct BeginHold {
    bool operator==(const BeginHold&) const { return true; }
};
struct EndHold {
    bool operator==(const EndHold&) const { return true; }
};
// Show the held framework placeholder as a "needs replug" card rather than
// removing it, so the device does not vanish without explanation.
struct MarkNeedsReplug {
    bool operator==(const MarkNeedsReplug&) const { return true; }
};
// Show the held synthetic placeholder as a card whose toggle stays live, so the
// user picks Direct, retry or replug instead of the app silently reverting.
struct MarkRestoreStuck {
    bool operator==(const MarkRestoreStuck&) const { return true; }
};
struct ClearRestoreStuck {
    bool operator==(const ClearRestoreStuck&) const { return true; }
};
struct StartTimeout {
    bool operator==(const StartTimeout&) const { return true; }
};
struct Notify {
    UsbNotice notice = UsbNotice::SwitchToDirectFailed;
    bool operator==(const Notify& o) const { return notice == o.notice; }
};
struct SetPref {
    PathChoice choice = PathChoice::Standard;
    bool operator==(const SetPref& o) const { return choice == o.choice; }
};
// Surfaces why Direct failed and suppresses auto-retry for the model. Cleared
// whenever a fresh attempt starts or Direct succeeds.
struct MarkFailure {
    DirectClaimFailure reason = DirectClaimFailure::Busy;
    bool operator==(const MarkFailure& o) const { return reason == o.reason; }
};
struct ClearFailure {
    bool operator==(const ClearFailure&) const { return true; }
};

} // namespace effect

using UsbEffect =
    std::variant<effect::Claim, effect::Reclaim, effect::Release, effect::RequestPermission,
                 effect::BindFramework, effect::RemoveSynthetic, effect::BeginHold, effect::EndHold,
                 effect::MarkNeedsReplug, effect::MarkRestoreStuck, effect::ClearRestoreStuck,
                 effect::StartTimeout, effect::Notify, effect::SetPref, effect::MarkFailure,
                 effect::ClearFailure>;

// next == nullopt means "remove this controller from tracking".
struct Reduction {
    std::optional<UsbController> next;
    std::vector<UsbEffect> effects;
};

Reduction reduce(const UsbController& c, const UsbEvent& event);

// Whether the coordinator should synthesize a FrameworkUp to settle a controller
// parked in AwaitingFramework.
//
// A claim does not remove the SDL twin, only twin-dedup
// suppresses it, so its vpKey stays continuously present and the coordinator's
// presence diff never sees a fresh appearance edge after a Direct-to-Standard
// Release. Waiting on that edge would wait forever. The rule is therefore
// level-triggered: settle if the framework device is present right now.
// `frameworkPresent` MUST be read from the real current device set, so a device
// that is genuinely gone keeps the Timeout path to RestoreStuck/NeedsReplug
// reachable. Settling is idempotent, since Routed is not AwaitingFramework.
inline bool shouldSettleAwaitingFramework(UsbPhase phase, bool frameworkPresent) {
    return phase == UsbPhase::AwaitingFramework && frameworkPresent;
}

// An explicit stored pick always wins. Under Auto, only a verified fast-lane
// model that has not just failed to claim goes Direct.
inline PathChoice resolvePathChoice(std::optional<PathChoice> stored, bool isFastLaneModel,
                                    std::optional<DirectClaimFailure> priorFailure) {
    if (stored.has_value()) { return *stored; }
    if (isFastLaneModel && !priorFailure.has_value()) { return PathChoice::Direct; }
    return PathChoice::Standard;
}

} // namespace dish::reducer
