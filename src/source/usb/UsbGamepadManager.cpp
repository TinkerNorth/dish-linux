// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/UsbGamepadManager.h"

#include "source/store/UsbPathPreferenceStore.h"

#include "Input/TouchGate.h"
#include "core/input/UsbReportParsers.h"
#include "core/model/Protocol.h"
#include "core/reducer/UsbClaimHealth.h"
#include "core/reducer/UsbPollRate.h"

#include "Input/GamepadInputProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
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

// The publish path a claimed pad's read loop runs, once per report, on the gateway thread. It is
// the SAME path the SDL bridge uses, so the routing table, deadzones and motion rate-limit all
// apply identically. The decoder already produced the XUSB button word, so there is no per-report
// conversion and no allocation here.
//
// A class rather than a lambda for the shape rules. The slot's processor index is resolved once,
// at the claim, so a report is published by index with no lookup.
class ClaimedPadPublisher {
  public:
    ClaimedPadPublisher(input::GamepadInputProcessor* processor, const std::string& slotId,
                        UsbDirectObserver* observer, int vendorId, int productId)
        : processor_(processor), observer_(observer), vendorId_(vendorId), productId_(productId) {
        if (processor_ != nullptr) { index_ = processor_->add(slotId); }
    }

    void operator()(const UsbReport& r) const {
        mirrorMicMute(r);
        mirrorBattery(r);
        if (processor_ == nullptr || !index_.has_value()) { return; }
        const input::GamepadInputProcessor::DeviceIndex index = *index_;
        publishInput(index, r);
        publishMotion(index, r);
        publishTouchpad(index, r);
    }

  private:
    // The decoder owns the mute latch (the wire bit is folded in on the read thread, with nothing
    // in the way); the observer is told only on the edge, so the mirror costs one relaxed load per
    // report. Fires on THIS thread, which is padMicMuteChanged's contract.
    void mirrorMicMute(const UsbReport& r) const {
        if (r.micMuted == lastMicMuted_->load(std::memory_order_relaxed)) { return; }
        lastMicMuted_->store(r.micMuted, std::memory_order_relaxed);
        if (observer_ != nullptr) {
            observer_->padMicMuteChanged(vendorId_, productId_, r.micMuted);
        }
    }

    // The charge the same way: the card shows it, AppModel's heartbeat puts the lower of it and
    // the host battery on the wire, and a level in tenths moves minutes apart.
    void mirrorBattery(const UsbReport& r) const {
        if (!r.batteryValid) { return; }
        const auto packed = static_cast<std::uint16_t>((r.batteryLevel << 8) | r.batteryStatus);
        if (packed == lastBattery_->load(std::memory_order_relaxed)) { return; }
        lastBattery_->store(packed, std::memory_order_relaxed);
        if (observer_ != nullptr) {
            observer_->padBatteryChanged(vendorId_, productId_, r.batteryLevel, r.batteryStatus);
        }
    }

    void publishInput(input::GamepadInputProcessor::DeviceIndex index, const UsbReport& r) const {
        input::GamepadInputProcessor::DeviceState st;
        st.wButtons = r.wButtons;
        st.lt = r.lt;
        st.rt = r.rt;
        st.lx = r.lx;
        st.ly = r.ly;
        st.rx = r.rx;
        st.ry = r.ry;
        processor_->publish(index, st);
    }

    // Rate-limited to <=250 Hz inside the processor.
    void publishMotion(input::GamepadInputProcessor::DeviceIndex index, const UsbReport& r) const {
        if (!r.motionValid) { return; }
        input::GamepadInputProcessor::MotionSample m;
        m.gyroX = r.gyroX;
        m.gyroY = r.gyroY;
        m.gyroZ = r.gyroZ;
        m.accelX = r.accelX;
        m.accelY = r.accelY;
        m.accelZ = r.accelZ;
        processor_->publishMotion(index, m);
    }

    // DS4 and DualSense only; every other decoder leaves touchpadValid false.
    void publishTouchpad(input::GamepadInputProcessor::DeviceIndex index,
                         const UsbReport& r) const {
        if (!r.touchpadValid) { return; }
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
        const input::TouchpadVerdict verdict = touchGate_->decide(t, processor_->nanoTime());
        if (verdict.send == input::TouchpadSend::Skip) { return; }
        t.eventTimeMs = verdict.eventTimeMs;
        processor_->publishTouchpad(index, t);
    }

    input::GamepadInputProcessor* processor_ = nullptr;
    std::optional<input::GamepadInputProcessor::DeviceIndex> index_;
    UsbDirectObserver* observer_ = nullptr;
    int vendorId_ = 0;
    int productId_ = 0;

    // Per claim, and shared across the copies std::function makes so the edge is tracked in one
    // place. Mic mute starts unmuted, which is what a freshly claimed pad's latch says too, so the
    // first edge is a real press and not a startup echo. The battery sentinel (status 0xFF) is a
    // value no report can produce, so the first valid reading is an edge and the card fills in at
    // once.
    std::shared_ptr<std::atomic<bool>> lastMicMuted_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<std::uint16_t>> lastBattery_ =
        std::make_shared<std::atomic<std::uint16_t>>(0xFFFF);
    // Touched only on the claim's raw-HID read thread.
    std::shared_ptr<input::TouchGate> touchGate_ = std::make_shared<input::TouchGate>();
};

} // namespace

UsbGamepadManager::UsbGamepadManager(UsbDeviceGateway* gateway,
                                     input::GamepadInputProcessor* processor,
                                     UsbPathPreferenceStore* prefs, UsbDirectObserver* observer)
    : gateway_(gateway), processor_(processor), prefs_(prefs), observer_(observer) {}

// Nothing else touches the manager while it is destroyed: a route table that still holds a worker
// holds it stopped.
UsbGamepadManager::~UsbGamepadManager() {
    for (auto& [syntheticId, worker] : workers_) { worker->stop(); }
}

void UsbGamepadManager::startWorker(int syntheticId, int key,
                                    const input::usbout::Ds5FirmwareInfo& ds5Firmware) {
    if (gateway_ == nullptr) { return; }
    const int productId = key & 0xFFFF;
    const auto parser = input::usbparse::parserForDevice(key >> 16, productId);
    const bool isDualSense = parser == input::usbparse::HidParser::DualSense;
    const bool vibrationV2 =
        isDualSense && input::usbout::ds5UsesVibrationV2(productId, ds5Firmware);
    auto worker = std::make_shared<OutputWorker>(gateway_->openReportWriter(syntheticId), parser,
                                                 vibrationV2);
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    workers_[syntheticId] = std::move(worker);
}

void UsbGamepadManager::releaseClaimAndWorker(int syntheticId) {
    std::shared_ptr<OutputWorker> worker;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        if (const auto it = workers_.find(syntheticId); it != workers_.end()) {
            worker = std::move(it->second);
            workers_.erase(it);
        }
    }
    if (worker != nullptr) { worker->stop(); }
    if (gateway_ != nullptr) { gateway_->releaseClaim(syntheticId); }
}

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
        if (claimEnded(device.vpKey())) { onUsbGone(device.vendorId, device.productId); }
        ensureTracked(device);
        judgeClaim(device.vpKey());
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
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
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
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
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

bool UsbGamepadManager::claimEnded(int key) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = controllers_.find(key);
    if (it == controllers_.end()) { return false; }
    const reducer::UsbController& c = it->second;
    if (c.phase != reducer::UsbPhase::Direct || !c.syntheticId.has_value()) { return false; }
    return !gateway_->isClaimAlive(*c.syntheticId);
}

void UsbGamepadManager::judgeClaim(int key) {
    reducer::ClaimActivity activity;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        const auto it = controllers_.find(key);
        if (it == controllers_.end()) {
            directScans_.erase(key);
            return;
        }
        const reducer::UsbController& c = it->second;
        if (c.phase != reducer::UsbPhase::Direct || !c.syntheticId.has_value()) {
            directScans_.erase(key);
            return;
        }
        const int syntheticId = *c.syntheticId;
        activity.reads = gateway_->completionCount(syntheticId);
        activity.decoded = gateway_->decodedCount(syntheticId);
        activity.scans = ++directScans_[key];
        activity.streamsUnprompted = input::usbparse::parserStreamsUnprompted(
            input::usbparse::parserForDevice(c.vendorId, c.productId));
    }
    if (reducer::judgeClaimHealth(activity) == reducer::ClaimHealth::Unreadable) {
        applyEvent(key, reducer::event::ClaimUnreadable{});
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
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    devices_.erase(key);
    missedScans_.erase(key);
    directScans_.erase(key);
    // A fresh plug-in of this model should re-evaluate Direct rather than inherit
    // a stale failure (android clears the registry's directFailed here).
    priorFailures_.erase(key);
}

void UsbGamepadManager::fireTimeout(int vendorId, int productId) {
    applyEvent(vpKey(vendorId, productId), reducer::event::Timeout{});
}

std::optional<UsbGamepadManager::DirectTarget>
UsbGamepadManager::directTarget(int vendorId, int productId) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = controllers_.find(vpKey(vendorId, productId));
    if (it == controllers_.end()) { return std::nullopt; }
    // Split from the lookup rather than folded into one `||`: a controller
    // being tracked and a controller holding a live claim are two different
    // facts, and separating them is also what lets the optional-access analysis
    // see that the dereference below is guarded.
    const reducer::UsbController& controller = it->second;
    if (!controller.syntheticId.has_value()) { return std::nullopt; }
    DirectTarget t;
    t.vendorId = vendorId;
    t.productId = productId;
    t.syntheticId = *controller.syntheticId;
    if (const auto w = workers_.find(t.syntheticId); w != workers_.end()) { t.worker = w->second; }
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
    return target.has_value() && applyRumble(*target, strongMagnitude, weakMagnitude);
}

bool UsbGamepadManager::applyRumble(const DirectTarget& target, std::uint16_t strongMagnitude,
                                    std::uint16_t weakMagnitude) {
    if (target.worker == nullptr || !input::usbparse::parserHasRumble(target.parser)) {
        return false;
    }
    target.worker->postRumble(strongMagnitude, weakMagnitude);
    return true;
}

bool UsbGamepadManager::applyLightbar(int vendorId, int productId, std::uint8_t r, std::uint8_t g,
                                      std::uint8_t b) {
    const auto target = directTarget(vendorId, productId);
    return target.has_value() && applyLightbar(*target, r, g, b);
}

bool UsbGamepadManager::applyLightbar(const DirectTarget& target, std::uint8_t r, std::uint8_t g,
                                      std::uint8_t b) {
    if (target.worker == nullptr || !input::usbout::parserHasLightbar(target.parser)) {
        return false;
    }
    target.worker->postLightbar(r, g, b);
    return true;
}

bool UsbGamepadManager::applyPlayerLeds(int vendorId, int productId, std::uint8_t ledMask) {
    const auto target = directTarget(vendorId, productId);
    return target.has_value() && applyPlayerLeds(*target, ledMask);
}

bool UsbGamepadManager::applyPlayerLeds(const DirectTarget& target, std::uint8_t ledMask) {
    if (target.worker == nullptr || !input::usbout::parserHasPlayerLeds(target.parser)) {
        return false;
    }
    target.worker->postPlayerLeds(ledMask);
    return true;
}

bool UsbGamepadManager::applyTriggerEffects(
    int vendorId, int productId, const std::uint8_t left[input::usbout::kTriggerEffectBlockBytes],
    const std::uint8_t right[input::usbout::kTriggerEffectBlockBytes]) {
    const auto target = directTarget(vendorId, productId);
    return target.has_value() && applyTriggerEffects(*target, left, right);
}

bool UsbGamepadManager::applyTriggerEffects(
    const DirectTarget& target, const std::uint8_t left[input::usbout::kTriggerEffectBlockBytes],
    const std::uint8_t right[input::usbout::kTriggerEffectBlockBytes]) {
    if (target.worker == nullptr || !input::usbout::parserHasTriggerEffects(target.parser)) {
        return false;
    }
    std::array<std::uint8_t, input::usbout::kTriggerEffectBlockBytes> l{};
    std::array<std::uint8_t, input::usbout::kTriggerEffectBlockBytes> r{};
    std::copy_n(left, l.size(), l.begin());
    std::copy_n(right, r.size(), r.begin());
    target.worker->postTriggerEffects(l, r);
    return true;
}

bool UsbGamepadManager::applyMicMuteLed(int vendorId, int productId, std::uint8_t state) {
    const auto target = directTarget(vendorId, productId);
    return target.has_value() && applyMicMuteLed(*target, state);
}

// MSG_MIC_LED's own values only; anything else is refused here, as the builder would.
bool UsbGamepadManager::applyMicMuteLed(const DirectTarget& target, std::uint8_t state) {
    if (target.worker == nullptr || !input::usbout::parserHasMicMuteLed(target.parser) ||
        state >= proto::kMicLedStateCount) {
        return false;
    }
    target.worker->postMicLed(state);
    return true;
}

bool UsbGamepadManager::setPadMicMuted(int vendorId, int productId, bool muted) {
    const auto target = directTarget(vendorId, productId);
    if (!target.has_value() || gateway_ == nullptr) { return false; }
    return gateway_->setPadMicMuted(target->syntheticId, muted);
}

std::map<int, reducer::UsbController> UsbGamepadManager::controllers() const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    return controllers_;
}

std::optional<reducer::UsbController> UsbGamepadManager::controllerFor(int vendorId,
                                                                       int productId) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = controllers_.find(vpKey(vendorId, productId));
    if (it == controllers_.end()) { return std::nullopt; }
    return it->second;
}

void UsbGamepadManager::applyEvent(int key, const reducer::UsbEvent& event) {
    std::optional<reducer::UsbController> cur;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        const auto it = controllers_.find(key);
        if (it == controllers_.end()) { return; }
        cur = it->second;
    }
    auto [next, effects] = reducer::reduce(*cur, event);
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
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
                if (c.syntheticId.has_value()) { releaseClaimAndWorker(*c.syntheticId); }
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
                releaseClaimAndWorker(e.syntheticId);
                if (observer_ != nullptr) { observer_->syntheticRemoved(e.syntheticId); }
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

std::optional<UsbDeviceInfo> UsbGamepadManager::trackedDevice(int key) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = devices_.find(key);
    if (it == devices_.end()) { return std::nullopt; }
    return it->second;
}

// A device gone between Choose and Claim is Busy with the framework not stolen, android's
// `null -> Busy` arm.
ClaimResult UsbGamepadManager::claimTracked(int key) {
    const std::optional<UsbDeviceInfo> device = trackedDevice(key);
    return device.has_value() ? doClaim(*device)
                              : ClaimResult::fail(reducer::DirectClaimFailure::Busy, false);
}

void UsbGamepadManager::runClaim(int key) {
    const ClaimResult outcome = claimTracked(key);
    if (outcome.ok) {
        startWorker(outcome.syntheticId, key, outcome.ds5Firmware);
        applyEvent(key, reducer::event::ClaimSucceeded{outcome.syntheticId});
    } else {
        applyEvent(key, reducer::event::ClaimFailed{outcome.reason, outcome.frameworkStolen});
    }
}

void UsbGamepadManager::runReclaim(int key, const reducer::UsbController& c) {
    const std::optional<int> oldPlaceholder = c.syntheticId;
    const ClaimResult outcome = claimTracked(key);
    // The old placeholder synthetic is dropped on a reclaim (android removes it
    // before feeding the outcome back).
    if (oldPlaceholder.has_value()) {
        releaseClaimAndWorker(*oldPlaceholder);
        if (observer_ != nullptr) { observer_->syntheticRemoved(*oldPlaceholder); }
    }
    if (outcome.ok) {
        startWorker(outcome.syntheticId, key, outcome.ds5Firmware);
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
    // The synthetic device id for the input pipeline is the model key as a string; one claimed pad
    // per model on this path.
    const ClaimResult outcome =
        gateway_->claim(device, ClaimedPadPublisher{processor_, std::to_string(device.vpKey()),
                                                    observer_, device.vendorId, device.productId});
    if (outcome.ok && observer_ != nullptr) {
        observer_->syntheticAdded(
            outcome.syntheticId, device.name, device.hasImu,
            reducer::computeUsbPollRateHz(device.endpointInInterval, device.endpointInMaxPacket),
            device.vendorId, device.productId);
    }
    return outcome;
}

} // namespace dish::source::usb
