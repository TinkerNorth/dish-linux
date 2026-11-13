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

#include "core/input/SteamTrackpads.h"
#include "source/usb/OutputWorker.h"
#include "source/usb/UsbDeviceGateway.h"

#include "core/input/UsbOutputReports.h"
#include "core/reducer/DirectPadKey.h"
#include "core/reducer/UsbPathMachine.h"
#include "core/thread/TrackedMutex.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
    // A synthetic pad came onto the slot list / left it: a model's own pad with
    // its claim, a receiver's slot while its controller is connected.
    virtual void syntheticAdded(int /*syntheticId*/, const std::string& /*name*/, bool /*hasGyro*/,
                                int /*pollRateHz*/, int /*vendorId*/, int /*productId*/) {}
    virtual void syntheticRemoved(int /*syntheticId*/) {}
    // The claimed pad's own mic-mute latch moved (the DualSense button, or a
    // setPadMicMuted write echoing back). UNLIKE the other callbacks, this one
    // fires on the GATEWAY READ THREAD — the edge is seen where the reports
    // are — so an observer must marshal to its own thread before touching
    // state (AppModel queues it).
    virtual void padMicMuteChanged(const reducer::DirectPadKey& /*pad*/, bool /*muted*/) {}
    // The claimed pad's own charge moved (usbparse::kPadBattery* values). Same
    // thread and same edge rule as padMicMuteChanged: once per change, on the
    // gateway read thread, never once per report.
    virtual void padBatteryChanged(const reducer::DirectPadKey& /*pad*/, std::uint8_t /*level*/,
                                   std::uint8_t /*status*/) {}
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
    // Stops every claim's output worker; the gateway, which closes the handles, outlives this.
    ~UsbGamepadManager();

    UsbGamepadManager(const UsbGamepadManager&) = delete;
    UsbGamepadManager& operator=(const UsbGamepadManager&) = delete;
    UsbGamepadManager(UsbGamepadManager&&) = delete;
    UsbGamepadManager& operator=(UsbGamepadManager&&) = delete;

    // Re-scan the USB bus: (idempotently) start tracking present pads, driving
    // each toward its resolved path automatically (not user-initiated), and
    // forget tracked pads that no longer enumerate (the departed-device sweep —
    // the polled unplug signal, since hidraw has no detach broadcast).
    // The sweep is debounced: a pad must miss kDepartedScanThreshold consecutive
    // scans before it reads as unplugged, so a single-pass enumeration hiccup
    // (Bluetooth link parking, a momentary open elsewhere) can never tear down a
    // live claim. A receiver's slot whose controller connected or disconnected
    // since the last scan joins or leaves directPads() here. Safe to call
    // repeatedly (the 1 s poll).
    void reconcile();

    // The user explicitly picks Direct for a model (the Settings toggle / card
    // action). Persists the pick and drives a user-initiated Choose(Direct).
    // Every pick is the model's: each of its devices follows it, and a
    // receiver's slots share it.
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
    // The model's devices were physically unplugged.
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

    // Hand a claimed Steam Controller the slot's trackpad pick. False when the pad
    // is not Direct-claimed or has no trackpads.
    bool setTrackpadPick(const reducer::DirectPadKey& pad, input::usbparse::TrackpadPick pick);

    // A claimed pad's synthetic id, decoder family and output worker, resolved once so a caller
    // that keeps it (the published route table) posts feedback with no lookup. A target whose claim
    // has since ended posts to a stopped worker, which writes nothing. By model, the model's own
    // pad; a receiver's slots go by their pad key.
    struct DirectTarget {
        int vendorId = 0;
        int productId = 0;
        int syntheticId = 0;
        input::usbparse::HidParser parser = input::usbparse::HidParser::None;
        std::shared_ptr<OutputWorker> worker;
    };
    std::optional<DirectTarget> directTarget(const reducer::DirectPadKey& pad) const;
    std::optional<DirectTarget> directTarget(int vendorId, int productId) const;
    bool applyRumble(const DirectTarget& target, std::uint16_t strongMagnitude,
                     std::uint16_t weakMagnitude);
    bool applyLightbar(const DirectTarget& target, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    bool applyPlayerLeds(const DirectTarget& target, std::uint8_t ledMask);
    bool applyTriggerEffects(const DirectTarget& target,
                             const std::uint8_t left[input::usbout::kTriggerEffectBlockBytes],
                             const std::uint8_t right[input::usbout::kTriggerEffectBlockBytes]);
    bool applyMicMuteLed(const DirectTarget& target, std::uint8_t state);

    // Whether a Direct claim for this pad is live right now. The link layer of
    // the capability solve asks this before advertising an actuator, so the
    // descriptor never promises a surface that a Standard-path pad cannot land.
    bool isDirectClaimed(const reducer::DirectPadKey& pad) const;
    bool isDirectClaimed(int vendorId, int productId) const;
    bool setPadMicMuted(const reducer::DirectPadKey& pad, bool muted);

    // The live FSM state per model (vpKey -> controller), as the model's first
    // device has it: each device of a model runs its own. Read-only snapshot.
    std::map<int, reducer::UsbController> controllers() const;
    std::optional<reducer::UsbController> controllerFor(int vendorId, int productId) const;

    // One pad on the slot list: a Direct model's own pad, or a slot of a Direct
    // receiver whose controller is connected. A receiver's other slots stay
    // claimed, listening for their controllers, and are no pad.
    struct DirectPad {
        reducer::DirectPadKey key;
        int syntheticId = 0;
    };
    // Read-only snapshot, in key order.
    std::vector<DirectPad> directPads() const;

  private:
    static int vpKey(int vendorId, int productId) {
        return (vendorId << 16) | (productId & 0xFFFF);
    }

    // One pad a claim reads: a model's own pad, or one slot of a receiver. `lead` is the
    // synthetic id the FSM holds for the claim the pad belongs to, which takes every pad of the
    // claim with it. Kept through a Release as the held placeholder, until RemoveSynthetic.
    struct PadClaim {
        reducer::DirectPadKey pad;
        UsbDeviceInfo device;
        int lead = 0;
        std::shared_ptr<OutputWorker> worker;
        // Whether a controller is there, as the last scan saw it (UsbDeviceGateway::isPadLinked).
        bool linked = false;
    };

    // Resolve a device's desired path from its model's stored pick and fast-lane
    // verdict and its own prior failure (the pure resolvePathChoice policy).
    reducer::PathChoice resolvePath(const reducer::DirectPadKey& device) const;

    // The key a scanned device goes by (reducer::deviceKeyOf): the one it is
    // tracked under, or a new one on the first unit of its model no tracked
    // device holds.
    reducer::DirectPadKey deviceKeyFor(const UsbDeviceInfo& device) const;
    std::optional<reducer::DirectPadKey> trackedDeviceKey(const UsbDeviceInfo& device) const;
    int firstFreeUnit(int vendorId, int productId) const;

    // Ensure a controller entry exists for a present device and record its pads
    // (the device's one, or each slot of a receiver), then drive a new entry
    // toward its resolved path (not user-initiated). Idempotent. `pads` is one
    // device's from one scan, never empty.
    void ensureTracked(const reducer::DirectPadKey& device, const std::vector<UsbDeviceInfo>& pads);
    // A pick made before the first scan tracks the model's devices first.
    void trackPresent(int vendorId, int productId);
    // A model-wide event (a pick, the framework device, the timer) reaches each
    // of the model's devices.
    std::vector<reducer::DirectPadKey> devicesOf(int vendorId, int productId) const;
    void applyToModel(int vendorId, int productId, const reducer::UsbEvent& event);

    // A Direct controller whose read loop has ended while its device still
    // enumerates: the pad re-enumerated where it was, which the departed-device
    // sweep cannot see.
    bool claimEnded(const reducer::DirectPadKey& device) const;

    // Judge a Direct claim's read and decode counts once per scan and release
    // one that reads nothing it can use.
    void judgeClaim(const reducer::DirectPadKey& device);
    bool padReadsNothing(int syntheticId, const PadClaim& claim);

    // Forget tracked devices that missed kDepartedScanThreshold scans in a row.
    void sweepDeparted(const std::set<reducer::DirectPadKey>& present);
    // A device left the bus: its pads go, and what was learnt about it with them.
    void deviceGone(const reducer::DirectPadKey& device);
    // Put the receivers' slots whose controllers came or went on the slot list or off it.
    void followPadLinks();
    void announcePad(int syntheticId, const PadClaim& claim);
    void listPad(int syntheticId, const UsbDeviceInfo& device);
    void unlistPad(int syntheticId);

    // Run the reducer for one event and execute its effects. All FSM mutation
    // funnels through here so events apply in order. `claimEnd` is how a
    // RemoveSynthetic ends its pads' claims: gone with the device on an
    // unplug, handed back on every other way out of Direct.
    void applyEvent(const reducer::DirectPadKey& device, const reducer::UsbEvent& event);
    void execute(const reducer::DirectPadKey& device, const reducer::UsbController& c,
                 const reducer::UsbEffect& fx, ClaimEnd claimEnd);

    // One pad as the claim opened it: the device, the gateway's answer and the worker made for it
    // before the claim, so the read thread had somewhere to post to from its first report on.
    struct OpenedPad {
        reducer::DirectPadKey pad;
        UsbDeviceInfo device;
        ClaimResult outcome;
        std::shared_ptr<OutputWorker> worker;
    };
    OpenedPad openPad(const reducer::DirectPadKey& pad, const UsbDeviceInfo& device);
    // A pad's output worker is made with its claim and stopped before the claim is released, so a
    // replugged pad starts its feedback state (the DualSense handoff, the lamp) afresh, in the
    // rumble mode the claim's firmware read picked. Started here, with the claim's writer; a
    // model's own pad is told it is there, which a receiver's slot learns from its link events.
    void startWorker(const OpenedPad& pad);
    void recordPad(int lead, const OpenedPad& pad);
    void releasePad(int syntheticId, ClaimEnd end);
    bool forgetPad(int syntheticId);
    // Every pad of the claim `lead` leads: handed back and kept as the held placeholder (Release),
    // or dropped (RemoveSynthetic), handed back too unless the device is gone, which gets nothing
    // written to it.
    void releasePads(int lead);
    void dropPads(int lead, ClaimEnd end);
    std::vector<int> padsOfClaim(int lead) const;
    bool leadsDirectClaim(const PadClaim& claim) const;

    // Effectors. Each looks the device up under mtx_ and claims and applies the outcome after
    // releasing it, because applyEvent takes mtx_ itself.
    struct TrackedPad {
        reducer::DirectPadKey pad;
        UsbDeviceInfo device;
    };
    std::vector<TrackedPad> trackedPads(const reducer::DirectPadKey& device) const;
    ClaimResult claimPads(const reducer::DirectPadKey& device);
    void letGo(const std::vector<OpenedPad>& opened);
    void runClaim(const reducer::DirectPadKey& device);
    void runReclaim(const reducer::DirectPadKey& device, const reducer::UsbController& c);
    // Open + claim + read-loop bring-up; classify the outcome. The single place
    // an open/claim outcome becomes a DirectClaimFailure (the UsbGamepadManager
    // tests pin this classification). `worker` is where the claim's read thread
    // posts the pad's arrival and departure.
    ClaimResult doClaim(const reducer::DirectPadKey& pad, const UsbDeviceInfo& device,
                        const std::shared_ptr<OutputWorker>& worker);

    UsbDeviceGateway* gateway_;
    input::GamepadInputProcessor* processor_;
    UsbPathPreferenceStore* prefs_;
    UsbDirectObserver* observer_;

    mutable thread::TrackedMutex mtx_{"usb.manager"}; // guards the maps below.
    // The path FSM per device, by its device key.
    std::map<reducer::DirectPadKey, reducer::UsbController> controllers_;
    // The last-known device descriptor per tracked pad (for re-claim): a device's own, or each
    // slot of a receiver.
    std::map<reducer::DirectPadKey, UsbDeviceInfo> devices_;
    // In-memory prior-failure tracking per device (android keeps this on the
    // registry; we fold it in so the manager is self-contained).
    std::map<reducer::DirectPadKey, reducer::DirectClaimFailure> priorFailures_;
    // Consecutive reconcile() scans a tracked device has been missing from
    // enumerate(). Reset on sighting; the sweep fires at the threshold.
    std::map<reducer::DirectPadKey, int> missedScans_;
    // Scans a pad has been seen Direct with its controller there on, the claiming
    // scan included; the health judgement reads it and a claim that ends resets it.
    std::map<reducer::DirectPadKey, int> directScans_;

    // Every claimed pad, by its own synthetic id, under mtx_.
    std::map<int, PadClaim> claims_;
};

} // namespace dish::source::usb
