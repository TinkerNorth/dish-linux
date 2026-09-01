// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/UsbGamepadManager.h"

#include "source/store/UsbPathPreferenceStore.h"

#include "core/input/UsbReportParsers.h"
#include "core/reducer/UsbPollRate.h"

#include "Input/GamepadInputProcessor.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace dish::source::usb {

namespace {

// The default satellite controller type a restored Direct binding re-registers
// as when there was never an explicit type choice. Mirrors android's
// CONTROLLER_TYPE_XBOX default in bindTo. (0 == CONTROLLER_TYPE_XBOX, per
// satellite/src/core/types.h and proto::kControllerTypeXbox.)
constexpr int kControllerTypeXbox = 0;

// Consecutive reconcile() scans a tracked pad must be missing before the sweep
// declares it unplugged (~2 s at the 1 s poll). Same >=2-misses debounce shape
// as LinkState::Unstable's missed-ack rule.
constexpr int kDepartedScanThreshold = 2;

} // namespace

UsbGamepadManager::UsbGamepadManager(UsbDeviceGateway* gateway,
                                     input::GamepadInputProcessor* processor,
                                     UsbPathPreferenceStore* prefs, UsbDirectObserver* observer)
    : gateway_(gateway), processor_(processor), prefs_(prefs), observer_(observer) {}

reducer::PathChoice UsbGamepadManager::resolvePath(int vendorId, int productId) const {
    std::optional<reducer::PathChoice> stored;
    if (prefs_ != nullptr) { stored = prefs_->choiceFor(vendorId, productId); }
    const bool fastLane =
        gateway_ != nullptr && gateway_->isKnownFastLaneModel(vendorId, productId);
    std::optional<reducer::DirectClaimFailure> prior;
    const auto it = priorFailures_.find(vpKey(vendorId, productId));
    if (it != priorFailures_.end()) { prior = it->second; }
    return reducer::resolvePathChoice(stored, fastLane, prior);
}

void UsbGamepadManager::reconcile() {
    if (gateway_ == nullptr) { return; }
    std::unordered_set<int> presentKeys;
    for (const auto& device : gateway_->enumerate()) {
        presentKeys.insert(device.vpKey());
        ensureTracked(device);
    }
    // Departed-device sweep: a tracked model that stops enumerating has been
    // physically unplugged. This presence diff IS the unplug signal — the read
    // loop just breaks when its fd dies and this polled path has no detach
    // broadcast (android gets ACTION_USB_DEVICE_DETACHED). Debounced to
    // kDepartedScanThreshold
    // consecutive misses so one flaky scan (a Bluetooth link parking itself, a
    // transient exclusive open elsewhere) reads as a blip, not an unplug — a
    // false positive here would tear down and re-claim a perfectly live pad.
    std::vector<std::pair<int, int>> departed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& [key, c] : controllers_) {
            if (presentKeys.find(key) != presentKeys.end()) {
                missedScans_.erase(key);
            } else if (++missedScans_[key] >= kDepartedScanThreshold) {
                departed.emplace_back(c.vendorId, c.productId);
            }
        }
    }
    for (const auto& [vendorId, productId] : departed) { onUsbGone(vendorId, productId); }
}

void UsbGamepadManager::ensureTracked(const UsbDeviceInfo& device) {
    const int key = device.vpKey();
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        devices_[key] = device;
        if (controllers_.find(key) == controllers_.end()) {
            reducer::UsbController c;
            c.vendorId = device.vendorId;
            c.productId = device.productId;
            c.name = device.name;
            c.phase = reducer::UsbPhase::Routed;
            c.usbPresent = true;
            // hidraw has no runtime permission broker (unlike android's
            // UsbManager grant): access is a static udev rule, so a present pad
            // is always claim-eligible and a missing rule surfaces later as a
            // PermissionDenied claim failure.
            c.hasPermission = true;
            c.desired = resolvePath(device.vendorId, device.productId);
            c.frameworkExpected =
                input::usbparse::modelExpectsFrameworkGamepad(device.vendorId, device.productId);
            controllers_.emplace(key, std::move(c));
            isNew = true;
        }
    }
    if (isNew) {
        // Drive toward the resolved path automatically (not user-initiated).
        applyEvent(key, reducer::event::Choose{resolvePath(device.vendorId, device.productId),
                                               /*userInitiated=*/false});
    }
}

void UsbGamepadManager::tryDirectMode(int vendorId, int productId) {
    setPathChoice(vendorId, productId, reducer::PathChoice::Direct);
}

void UsbGamepadManager::setPathChoice(int vendorId, int productId, reducer::PathChoice choice) {
    if (prefs_ != nullptr) { prefs_->setChoice(vendorId, productId, choice); }
    // If the device is present but untracked (chosen before the first scan),
    // bring it in first so the Choose lands on a tracked controller.
    if (gateway_ != nullptr) {
        for (const auto& d : gateway_->enumerate()) {
            if (d.vendorId == vendorId && d.productId == productId) {
                ensureTracked(d);
                break;
            }
        }
    }
    applyEvent(vpKey(vendorId, productId), reducer::event::Choose{choice, /*userInitiated=*/true});
}

void UsbGamepadManager::clearChoice(int vendorId, int productId) {
    if (prefs_ != nullptr) { prefs_->clearChoice(vendorId, productId); }
    // Bring a present-but-untracked device in first (chosen before the first
    // scan), then re-resolve the path with the override now gone and drive it.
    if (gateway_ != nullptr) {
        for (const auto& d : gateway_->enumerate()) {
            if (d.vendorId == vendorId && d.productId == productId) {
                ensureTracked(d);
                break;
            }
        }
    }
    applyEvent(vpKey(vendorId, productId),
               reducer::event::Choose{resolvePath(vendorId, productId), /*userInitiated=*/true});
}

void UsbGamepadManager::onFrameworkUp(int vendorId, int productId, int frameworkId) {
    applyEvent(vpKey(vendorId, productId), reducer::event::FrameworkUp{frameworkId});
}

void UsbGamepadManager::onFrameworkDown(int vendorId, int productId) {
    applyEvent(vpKey(vendorId, productId), reducer::event::FrameworkDown{});
}

void UsbGamepadManager::onUsbGone(int vendorId, int productId) {
    const int key = vpKey(vendorId, productId);
    applyEvent(key, reducer::event::UsbUnplugged{});
    std::lock_guard<std::mutex> lock(mtx_);
    devices_.erase(key);
    missedScans_.erase(key);
    // A fresh plug-in of this model should re-evaluate Direct rather than inherit
    // a stale failure (android clears the registry's directFailed here).
    priorFailures_.erase(key);
}

void UsbGamepadManager::fireTimeout(int vendorId, int productId) {
    applyEvent(vpKey(vendorId, productId), reducer::event::Timeout{});
}

void UsbGamepadManager::forgetFeedbackState(int key) {
    std::lock_guard<std::mutex> lock(feedbackMtx_);
    feedback_.erase(key);
    feedbackSeq_.erase(key);
}

std::optional<UsbGamepadManager::DirectTarget>
UsbGamepadManager::directTarget(int vendorId, int productId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = controllers_.find(vpKey(vendorId, productId));
    if (it == controllers_.end() || !it->second.syntheticId.has_value()) { return std::nullopt; }
    DirectTarget t;
    t.syntheticId = *it->second.syntheticId;
    // The family comes from the identity, not from the gateway: the manager
    // must answer the same question the capability solve asked when it built
    // the descriptor, and that one only ever had (vid, pid).
    t.parser = input::usbparse::parserForDevice(vendorId, productId);
    return t;
}

bool UsbGamepadManager::isDirectClaimed(int vendorId, int productId) const {
    return directTarget(vendorId, productId).has_value();
}

bool UsbGamepadManager::applyRumble(int vendorId, int productId, std::uint16_t strongMagnitude,
                                    std::uint16_t weakMagnitude) {
    const auto target = directTarget(vendorId, productId);
    if (!target.has_value() || gateway_ == nullptr) { return false; }
    std::array<std::uint8_t, input::usbout::kMaxOutputReportBytes> buf{};
    std::uint8_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(feedbackMtx_);
        seq = nextSeqLocked(vpKey(vendorId, productId));
    }
    const std::size_t n = input::usbout::buildRumbleReport(
        target->parser, strongMagnitude, weakMagnitude, seq, buf.data(), buf.size());
    if (n == 0) { return false; }
    return gateway_->writeOutputReport(target->syntheticId, buf.data(), n);
}

bool UsbGamepadManager::applyLightbar(int vendorId, int productId, std::uint8_t r, std::uint8_t g,
                                      std::uint8_t b) {
    const auto target = directTarget(vendorId, productId);
    if (!target.has_value() || gateway_ == nullptr) { return false; }
    std::array<std::uint8_t, input::usbout::kMaxOutputReportBytes> buf{};
    std::size_t n = 0;
    {
        // The builder mutates the one-time DualSense handoff flag, so the state
        // lookup and the build are one critical section: two colours racing in
        // must not both decide they are the first.
        std::lock_guard<std::mutex> lock(feedbackMtx_);
        auto& st = feedback_[vpKey(vendorId, productId)];
        n = input::usbout::buildLightbarReport(target->parser, st, r, g, b, buf.data(), buf.size());
    }
    if (n == 0) { return false; }
    return gateway_->writeOutputReport(target->syntheticId, buf.data(), n);
}

bool UsbGamepadManager::applyPlayerLeds(int vendorId, int productId, std::uint8_t ledMask) {
    const auto target = directTarget(vendorId, productId);
    if (!target.has_value() || gateway_ == nullptr) { return false; }
    std::array<std::uint8_t, input::usbout::kMaxOutputReportBytes> buf{};
    std::uint8_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(feedbackMtx_);
        seq = nextSeqLocked(vpKey(vendorId, productId));
    }
    const std::size_t n =
        input::usbout::buildPlayerLedsReport(target->parser, ledMask, seq, buf.data(), buf.size());
    if (n == 0) { return false; }
    return gateway_->writeOutputReport(target->syntheticId, buf.data(), n);
}

bool UsbGamepadManager::applyTriggerEffects(
    int vendorId, int productId, const std::uint8_t left[input::usbout::kTriggerEffectBlockBytes],
    const std::uint8_t right[input::usbout::kTriggerEffectBlockBytes]) {
    const auto target = directTarget(vendorId, productId);
    if (!target.has_value() || gateway_ == nullptr) { return false; }
    std::array<std::uint8_t, input::usbout::kMaxOutputReportBytes> buf{};
    const std::size_t n = input::usbout::buildTriggerEffectsReport(target->parser, left, right,
                                                                   buf.data(), buf.size());
    if (n == 0) { return false; }
    return gateway_->writeOutputReport(target->syntheticId, buf.data(), n);
}

std::map<int, reducer::UsbController> UsbGamepadManager::controllers() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return controllers_;
}

std::optional<reducer::UsbController> UsbGamepadManager::controllerFor(int vendorId,
                                                                       int productId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = controllers_.find(vpKey(vendorId, productId));
    if (it == controllers_.end()) { return std::nullopt; }
    return it->second;
}

void UsbGamepadManager::applyEvent(int key, const reducer::UsbEvent& event) {
    std::optional<reducer::UsbController> cur;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = controllers_.find(key);
        if (it == controllers_.end()) { return; }
        cur = it->second;
    }
    auto [next, effects] = reducer::reduce(*cur, event);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (next.has_value()) {
            controllers_[key] = *next;
        } else {
            controllers_.erase(key);
        }
    }
    const reducer::UsbController& ctx = next.has_value() ? *next : *cur;
    for (const auto& fx : effects) { execute(key, ctx, fx); }

    // The single unidirectional notification: any FSM mutation re-renders the slot
    // list from the fresh snapshot. Emitted AFTER the effects (so the gateway
    // claim/release has run) and outside mtx_ (the observer reads controllers()).
    // This is what makes held-synthetic phase changes (Direct->AwaitingFramework)
    // reach the UI + trigger the AwaitingFramework settle.
    if (observer_ != nullptr) { observer_->controllersChanged(); }
}

void UsbGamepadManager::execute(int key, const reducer::UsbController& c,
                                const reducer::UsbEffect& fx) {
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, reducer::effect::Claim>) {
                runClaim(key);
            } else if constexpr (std::is_same_v<T, reducer::effect::Reclaim>) {
                runReclaim(key, c);
            } else if constexpr (std::is_same_v<T, reducer::effect::Release>) {
                if (c.syntheticId.has_value() && gateway_ != nullptr) {
                    gateway_->releaseClaim(*c.syntheticId);
                }
                forgetFeedbackState(key);
            } else if constexpr (std::is_same_v<T, reducer::effect::RequestPermission>) {
                // There is no runtime grant to request on hidraw — the open is
                // the only gate. Treat the request as immediately satisfied so a
                // user Choose(Direct) proceeds to the claim.
                applyEvent(key, reducer::event::PermissionGranted{});
            } else if constexpr (std::is_same_v<T, reducer::effect::BindFramework>) {
                if (observer_ != nullptr && c.connId.has_value()) {
                    observer_->bind(e.frameworkId, *c.connId, c.type.value_or(kControllerTypeXbox));
                }
            } else if constexpr (std::is_same_v<T, reducer::effect::RemoveSynthetic>) {
                if (gateway_ != nullptr) { gateway_->releaseClaim(e.syntheticId); }
                if (observer_ != nullptr) { observer_->syntheticRemoved(e.syntheticId); }
                forgetFeedbackState(key);
            } else if constexpr (std::is_same_v<T, reducer::effect::BeginHold>) {
                if (observer_ != nullptr) { observer_->beginHold(c.vendorId, c.productId); }
            } else if constexpr (std::is_same_v<T, reducer::effect::EndHold>) {
                if (observer_ != nullptr) { observer_->endHold(c.vendorId, c.productId); }
            } else if constexpr (std::is_same_v<T, reducer::effect::MarkNeedsReplug>) {
                if (observer_ != nullptr) { observer_->markNeedsReplug(c.vendorId, c.productId); }
            } else if constexpr (std::is_same_v<T, reducer::effect::MarkRestoreStuck>) {
                if (observer_ != nullptr) { observer_->markRestoreStuck(c.vendorId, c.productId); }
            } else if constexpr (std::is_same_v<T, reducer::effect::ClearRestoreStuck>) {
                if (observer_ != nullptr) { observer_->clearRestoreStuck(c.vendorId, c.productId); }
            } else if constexpr (std::is_same_v<T, reducer::effect::StartTimeout>) {
                // The real transition timer is owned by the production wiring
                // (a Qt timer fed back through fireTimeout). Nothing to do in the
                // pure driver core; tests fire the timeout deterministically.
            } else if constexpr (std::is_same_v<T, reducer::effect::Notify>) {
                if (observer_ != nullptr) { observer_->notice(c, e.notice); }
            } else if constexpr (std::is_same_v<T, reducer::effect::SetPref>) {
                if (prefs_ != nullptr) { prefs_->setChoice(c.vendorId, c.productId, e.choice); }
            } else if constexpr (std::is_same_v<T, reducer::effect::MarkFailure>) {
                priorFailures_[key] = e.reason;
                if (observer_ != nullptr) {
                    observer_->markFailure(c.vendorId, c.productId, e.reason);
                }
            } else if constexpr (std::is_same_v<T, reducer::effect::ClearFailure>) {
                priorFailures_.erase(key);
                if (observer_ != nullptr) { observer_->clearFailure(c.vendorId, c.productId); }
            }
        },
        fx);
}

void UsbGamepadManager::runClaim(int key) {
    UsbDeviceInfo device;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = devices_.find(key);
        if (it == devices_.end()) {
            // No device to claim (gone between Choose and Claim): treat as Busy,
            // framework not stolen — mirrors android's `null -> Busy` arm.
            applyEvent(key, reducer::event::ClaimFailed{reducer::DirectClaimFailure::Busy, false});
            return;
        }
        device = it->second;
    }
    const ClaimResult outcome = doClaim(device);
    if (outcome.ok) {
        applyEvent(key, reducer::event::ClaimSucceeded{outcome.syntheticId});
    } else {
        applyEvent(key, reducer::event::ClaimFailed{outcome.reason, outcome.frameworkStolen});
    }
}

void UsbGamepadManager::runReclaim(int key, const reducer::UsbController& c) {
    const std::optional<int> oldPlaceholder = c.syntheticId;
    UsbDeviceInfo device;
    bool haveDevice = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = devices_.find(key);
        if (it != devices_.end()) {
            device = it->second;
            haveDevice = true;
        }
    }
    const ClaimResult outcome =
        haveDevice ? doClaim(device) : ClaimResult::fail(reducer::DirectClaimFailure::Busy, false);
    // The old placeholder synthetic is dropped on a reclaim (android removes it
    // before feeding the outcome back).
    if (oldPlaceholder.has_value()) {
        if (gateway_ != nullptr) { gateway_->releaseClaim(*oldPlaceholder); }
        if (observer_ != nullptr) { observer_->syntheticRemoved(*oldPlaceholder); }
    }
    if (outcome.ok) {
        if (observer_ != nullptr && c.connId.has_value()) {
            observer_->bind(outcome.syntheticId, *c.connId, c.type.value_or(kControllerTypeXbox));
        }
        applyEvent(key, reducer::event::ClaimSucceeded{outcome.syntheticId});
    } else {
        applyEvent(key, reducer::event::ClaimFailed{outcome.reason, outcome.frameworkStolen});
    }
}

ClaimResult UsbGamepadManager::doClaim(const UsbDeviceInfo& device) {
    if (gateway_ == nullptr) {
        return ClaimResult::fail(reducer::DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    }
    // The read loop publishes each decoded XUSB report to GamepadInputProcessor
    // on the gateway thread — the SAME publish path the SDL bridge uses (so the
    // routing table, deadzones, and motion rate-limit all apply identically). The
    // decoder already produced the XUSB button word, so there is no per-report
    // conversion and no allocation here; INPUT goes through publish(), MOTION
    // through publishMotion() (rate-limited to <=250 Hz by the processor), and the
    // DS4/DualSense TOUCHPAD through publishTouchpad().
    const std::string deviceTag = device.name;
    const int vp = device.vpKey();
    // Not const on purpose: a by-copy capture carries the captured entity's
    // cv-qualifiers into the closure, so a `const std::string` local becomes a
    // const closure member — and a const member forces the closure's implicit
    // MOVE constructor to fall back to std::string's copy ctor. That move is how
    // the lambda gets into the gateway's std::function, so const here would buy a
    // throwing move ctor plus a heap copy of the slot id on every claim.
    std::string slotId = std::to_string(vp);
    input::GamepadInputProcessor* processor = processor_;
    const ClaimResult outcome = gateway_->claim(device, [processor, slotId](const UsbReport& r) {
        if (processor == nullptr) { return; }
        input::GamepadInputProcessor::DeviceState st;
        st.wButtons = r.wButtons;
        st.lt = r.lt;
        st.rt = r.rt;
        st.lx = r.lx;
        st.ly = r.ly;
        st.rx = r.rx;
        st.ry = r.ry;
        // The synthetic device id for the input pipeline is the model key as a
        // string; one claimed pad per model on this path.
        processor->publish(slotId, st);
        if (r.motionValid) {
            input::GamepadInputProcessor::MotionSample m;
            m.gyroX = r.gyroX;
            m.gyroY = r.gyroY;
            m.gyroZ = r.gyroZ;
            m.accelX = r.accelX;
            m.accelY = r.accelY;
            m.accelZ = r.accelZ;
            processor->publishMotion(slotId, m);
        }
        if (r.touchpadValid) {
            input::GamepadInputProcessor::TouchpadSample t;
            t.finger0Active = r.finger0Active;
            t.finger0Id = r.finger0Id;
            t.finger0X = r.finger0X;
            t.finger0Y = r.finger0Y;
            t.finger1Active = r.finger1Active;
            t.finger1Id = r.finger1Id;
            t.finger1X = r.finger1X;
            t.finger1Y = r.finger1Y;
            t.buttonPressed = r.touchpadButton;
            processor->publishTouchpad(slotId, t);
        }
    });
    if (outcome.ok && observer_ != nullptr) {
        observer_->syntheticAdded(
            outcome.syntheticId, deviceTag, device.hasImu,
            reducer::computeUsbPollRateHz(device.endpointInInterval, device.endpointInMaxPacket),
            device.vendorId, device.productId);
    }
    return outcome;
}

} // namespace dish::source::usb
