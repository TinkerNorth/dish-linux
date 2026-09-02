// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The USB-direct claim driver: the imperative coordinator that executes the
// effects the pure UsbPathMachine FSM decides. This is the only place that
// performs USB IO.
//
// xpad binds Xbox-class pads as evdev with no hidraw node, so they stay on the
// SDL path; this path serves HID pads (DualSense, DualShock 4, Switch Pro,
// 8BitDo). A model that fails to claim falls back to SDL, so USB-direct is
// strictly additive and never a regression.
//
// Hot-path discipline: the Gateway read loop feeds decoded reports straight into
// GamepadInputProcessor::publish, the same entry point the SDL bridge uses, with
// no Qt signals on the per-report path. All FSM mutation is serialized through
// applyEvent so events apply in order.

#pragma once

#include "source/usb/UsbDeviceGateway.h"

#include "core/input/UsbOutputReports.h"
#include "core/reducer/UsbPathMachine.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace dish::input {
class GamepadInputProcessor;
}

namespace dish::source {
class UsbPathPreferenceStore;
}

namespace dish::source::usb {

// The non-input side-effects the FSM produces, surfaced for the UI/registry to
// render. The C++ analogue of the android registry+notification calls the
// effector makes (markDirectFailed, beginModelTransition, markNeedsReplug,
// notify(...), ...). Default no-op so headless/test wiring can ignore them.
class UsbDirectObserver {
  public:
    virtual ~UsbDirectObserver() = default;

    // A user-facing banner reason fired (the UI maps notice->localized string).
    virtual void notice(const reducer::UsbController& /*c*/, reducer::UsbNotice /*notice*/) {}
    // The controller card's persistent state marks.
    virtual void markFailure(int /*vendorId*/, int /*productId*/,
                             reducer::DirectClaimFailure /*reason*/) {}
    virtual void clearFailure(int /*vendorId*/, int /*productId*/) {}
    virtual void markNeedsReplug(int /*vendorId*/, int /*productId*/) {}
    virtual void markRestoreStuck(int /*vendorId*/, int /*productId*/) {}
    virtual void clearRestoreStuck(int /*vendorId*/, int /*productId*/) {}
    virtual void beginHold(int /*vendorId*/, int /*productId*/) {}
    virtual void endHold(int /*vendorId*/, int /*productId*/) {}
    // Bind the carried connection to a device id (framework or synthetic).
    virtual void bind(int /*deviceId*/, const std::string& /*connId*/, int /*type*/) {}
    // A synthetic device was registered / removed (the UI shows it as a slot).
    virtual void syntheticAdded(int /*syntheticId*/, const std::string& /*name*/, bool /*hasGyro*/,
                                int /*pollRateHz*/, int /*vendorId*/, int /*productId*/) {}
    virtual void syntheticRemoved(int /*syntheticId*/) {}
    // The claimed pad's own mic-mute latch moved (the DualSense button, or a
    // setPadMicMuted write echoing back). UNLIKE the other callbacks, this one
    // fires on the GATEWAY READ THREAD — the edge is seen where the reports
    // are — so an observer must marshal to its own thread before touching
    // state (AppModel queues it).
    virtual void padMicMuteChanged(int /*vendorId*/, int /*productId*/, bool /*muted*/) {}
    // Fired once after EVERY state-changing applyEvent. This is the single
    // unidirectional "the FSM state moved, rebuild the slot list from the fresh
    // controllers() snapshot" signal — the slot list binds to this, not to the
    // granular per-effect callbacks above (which exist for banners/toasts). It is
    // what makes a held-synthetic phase change (e.g. Direct->AwaitingFramework on a
    // Standard pick) reach the UI; without it those transitions were invisible.
    virtual void controllersChanged() {}
};

class UsbGamepadManager {
  public:
    // `gateway` is the IO seam (real HidrawGateway in production, a fake in
    // tests). `processor` receives decoded reports on the gateway read thread
    // (may be null in tests that only assert FSM/claim outcomes). `prefs` is the
    // per-VID:PID path store (may be null; then Auto resolution always applies).
    // `observer` receives the non-input effects (may be null -> a no-op).
    // `nextSyntheticId` lets a test pin the synthetic id the gateway-less path
    // would assign; production uses the gateway's returned id.
    UsbGamepadManager(UsbDeviceGateway* gateway, input::GamepadInputProcessor* processor,
                      UsbPathPreferenceStore* prefs, UsbDirectObserver* observer);

    // Re-scan the USB bus: (idempotently) start tracking present pads, driving
    // each toward its resolved path automatically (not user-initiated), and
    // forget tracked pads that no longer enumerate (the departed-device sweep —
    // the polled unplug signal, since hidraw has no detach broadcast).
    // The sweep is debounced: a pad must miss kDepartedScanThreshold consecutive
    // scans before it reads as unplugged, so a single-pass enumeration hiccup
    // (Bluetooth link parking, a momentary open elsewhere) can never tear down a
    // live claim. Safe to call repeatedly (the 1 s poll).
    void reconcile();

    // The user explicitly picks Direct for a model (the Settings toggle / card
    // action). Persists the pick and drives a user-initiated Choose(Direct).
    void tryDirectMode(int vendorId, int productId);

    // The user explicitly picks `choice` for a model. Persists + drives a
    // user-initiated Choose.
    void setPathChoice(int vendorId, int productId, reducer::PathChoice choice);

    // The user picks Auto: drop the stored override and drive a user-initiated
    // Choose with the freshly RE-RESOLVED path (the resolution policy now decides
    // with no stored pick — a fast-lane model returns to Direct, everything else
    // to Standard).
    void clearChoice(int vendorId, int productId);

    // ── World-signal entry points (the driver turns these into events) ────────
    // Framework (SDL) device for this model appeared / disappeared.
    void onFrameworkUp(int vendorId, int productId, int frameworkId);
    void onFrameworkDown(int vendorId, int productId);
    // The device was physically unplugged.
    void onUsbGone(int vendorId, int productId);
    // The transition timer for a model elapsed (the driver normally owns the
    // timer; exposed so a test can fire it deterministically).
    void fireTimeout(int vendorId, int productId);

    // ── Feedback to a Direct-claimed pad (the OUT direction) ─────────────────
    //
    // Keyed on the model, like every other public entry point here: a slot id
    // carries (vid, pid), and the manager owns the mapping from that to the live
    // synthetic id. Each returns whether the report actually reached the device,
    // so a caller can tell "the pad has no such actuator" from "the pad has one
    // and it fired" — the descriptor's caps are built from the same predicates,
    // so in practice a false here means the claim went away mid-flight.
    //
    // Callable from the network receive thread. No-ops (false) when the model is
    // not Direct-claimed right now, which is the normal case for a pad the user
    // left on the Standard path.
    bool applyRumble(int vendorId, int productId, std::uint16_t strongMagnitude,
                     std::uint16_t weakMagnitude);
    bool applyLightbar(int vendorId, int productId, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    bool applyPlayerLeds(int vendorId, int productId, std::uint8_t ledMask);
    bool applyTriggerEffects(int vendorId, int productId,
                             const std::uint8_t left[input::usbout::kTriggerEffectBlockBytes],
                             const std::uint8_t right[input::usbout::kTriggerEffectBlockBytes]);
    // The mic-mute lamp. `state` is MSG_MIC_LED's own value (0/1/2); the
    // builder records it in the FeedbackState shadow so every later DS5 write
    // re-asserts it instead of stomping it.
    bool applyMicMuteLed(int vendorId, int productId, std::uint8_t state);

    // Overwrite the claimed pad's mic-mute latch (the UI mute control's way
    // into the wire bit). False when the model is not Direct-claimed or has no
    // latch.
    bool setPadMicMuted(int vendorId, int productId, bool muted);

    // Whether a Direct claim for this model is live right now. The link layer of
    // the capability solve asks this before advertising an actuator, so the
    // descriptor never promises a surface that a Standard-path pad cannot land.
    bool isDirectClaimed(int vendorId, int productId) const;

    // The live FSM state per model (vpKey -> controller). Read-only snapshot.
    std::map<int, reducer::UsbController> controllers() const;
    std::optional<reducer::UsbController> controllerFor(int vendorId, int productId) const;

  private:
    static int vpKey(int vendorId, int productId) {
        return (vendorId << 16) | (productId & 0xFFFF);
    }

    // Resolve a model's desired path from the stored pick + fast-lane + prior
    // failure (the pure resolvePathChoice policy).
    reducer::PathChoice resolvePath(int vendorId, int productId) const;

    // Ensure a controller entry exists for a present device, then drive it toward
    // its resolved path (not user-initiated). Idempotent.
    void ensureTracked(const UsbDeviceInfo& device);

    // Run the reducer for one event and execute its effects. All FSM mutation
    // funnels through here so events apply in order.
    void applyEvent(int key, const reducer::UsbEvent& event);
    void execute(int key, const reducer::UsbController& c, const reducer::UsbEffect& fx);

    // The claimed synthetic id + decoder family for a model, or nullopt when it
    // is not Direct-claimed. One lookup shared by every feedback entry point.
    struct DirectTarget {
        int syntheticId = 0;
        input::usbparse::HidParser parser = input::usbparse::HidParser::None;
    };
    std::optional<DirectTarget> directTarget(int vendorId, int productId) const;

    // Drop a model's OUT-direction state when its claim ends, so a replugged pad
    // re-sends the DualSense lightbar handoff instead of assuming the firmware
    // still remembers it. Takes feedbackMtx_.
    void forgetFeedbackState(int key);

    // The Switch Pro accepts a packet only when its low nibble advances. Caller
    // holds feedbackMtx_.
    std::uint8_t nextSeqLocked(int key) {
        const auto next = static_cast<std::uint8_t>((feedbackSeq_[key] + 1) & 0x0FU);
        feedbackSeq_[key] = next;
        return next;
    }

    // Effectors.
    void runClaim(int key);
    void runReclaim(int key, const reducer::UsbController& c);
    // Open + claim + read-loop bring-up; classify the outcome. The single place
    // an open/claim outcome becomes a DirectClaimFailure (the UsbGamepadManager
    // tests pin this classification).
    ClaimResult doClaim(const UsbDeviceInfo& device);

    UsbDeviceGateway* gateway_;
    input::GamepadInputProcessor* processor_;
    UsbPathPreferenceStore* prefs_;
    UsbDirectObserver* observer_;

    mutable std::mutex mtx_; // guards the maps below.
    std::map<int, reducer::UsbController> controllers_;
    // The last-known device descriptor per tracked model (for re-claim).
    std::unordered_map<int, UsbDeviceInfo> devices_;
    // In-memory prior-failure tracking (android keeps this on the registry; we
    // fold it in so the manager is self-contained). Cleared on a fresh plug-in.
    std::unordered_map<int, reducer::DirectClaimFailure> priorFailures_;
    // Consecutive reconcile() scans a tracked model has been missing from
    // enumerate(). Reset on sighting; the sweep fires at the threshold.
    std::unordered_map<int, int> missedScans_;

    // Per-model OUT-direction state, guarded by its own mutex rather than mtx_:
    // feedback arrives on the receive thread and must not contend with the 1 s
    // reconcile sweep. Holds the DualSense's one-time lightbar handoff flag and
    // the Switch Pro's rolling packet counter, both of which are properties of
    // the claim rather than of any one write. Erased when the claim ends, so a
    // replugged pad gets its handoff again.
    mutable std::mutex feedbackMtx_;
    std::unordered_map<int, input::usbout::FeedbackState> feedback_;
    std::unordered_map<int, std::uint8_t> feedbackSeq_;
};

} // namespace dish::source::usb
