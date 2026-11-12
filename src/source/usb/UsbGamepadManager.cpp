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

// The pad a device carries: a receiver's slot by its interface, any other model's own pad.
reducer::DirectPadKey padKeyOf(const UsbDeviceInfo& device) {
    const bool ridesASlot = input::usbparse::isReceiver(device.vendorId, device.productId);
    const int slot = ridesASlot ? device.interfaceNumber : reducer::kNoReceiverSlot;
    return reducer::DirectPadKey{device.vendorId, device.productId, slot};
}

int modelKeyOf(const reducer::DirectPadKey& pad) {
    return reducer::slotPathVpKey(pad.vendorId, pad.productId);
}

template <typename Value>
void eraseModelPads(std::map<reducer::DirectPadKey, Value>& byPad, int key) {
    std::erase_if(byPad, [key](const auto& entry) { return modelKeyOf(entry.first) == key; });
}

// The scan's devices, one group per model, in the order the scan first listed each model.
std::vector<std::vector<UsbDeviceInfo>> padsByModel(const std::vector<UsbDeviceInfo>& scan) {
    std::vector<std::vector<UsbDeviceInfo>> models;
    for (const UsbDeviceInfo& device : scan) {
        const auto model = std::find_if(models.begin(), models.end(), [&device](const auto& pads) {
            return pads.front().vpKey() == device.vpKey();
        });
        if (model == models.end()) {
            models.push_back({device});
            continue;
        }
        model->push_back(device);
    }
    return models;
}

// The publish path a claimed pad's read loop runs, once per report, on the gateway thread. It is
// the SAME path the SDL bridge uses, so the routing table, deadzones and motion rate-limit all
// apply identically. The decoder already produced the XUSB button word, so there is no per-report
// conversion and no allocation here.
//
// A class rather than a lambda for the shape rules. The slot's processor index is resolved once,
// at the claim, so a report is published by index with no lookup.
class ClaimedPadPublisher {
  public:
    ClaimedPadPublisher(input::GamepadInputProcessor* processor, UsbDirectObserver* observer,
                        const reducer::DirectPadKey& pad)
        : processor_(processor), observer_(observer), pad_(pad) {
        if (processor_ != nullptr) { index_ = processor_->add(reducer::directSlotId(pad_)); }
    }

    void operator()(const UsbReport& r) const {
        switch (r.linkEvent) {
        case PadLinkEvent::None:
            publish(r);
            return;
        case PadLinkEvent::ControllerLeft:
            controllerLeft();
            return;
        }
    }

  private:
    // The battery sentinel (status 0xFF) is a value no report can produce, so the first valid
    // reading is an edge and the card fills in at once.
    static constexpr std::uint16_t kNoBatteryYet = 0xFFFF;

    void publish(const UsbReport& r) const {
        mirrorMicMute(r);
        mirrorBattery(r);
        if (processor_ == nullptr || !index_.has_value()) { return; }
        const input::GamepadInputProcessor::DeviceIndex index = *index_;
        publishInput(index, r);
        publishMotion(index, r);
        publishTouchpad(index, r);
    }

    // A departing controller's last input must not stay latched, and its battery reading leaves
    // with it: the card forgets the reading with the pad, so the first one after the controller
    // returns is an edge again whatever its level.
    void controllerLeft() const {
        publish(UsbReport{});
        lastBattery_->store(kNoBatteryYet, std::memory_order_relaxed);
    }

    // The decoder owns the mute latch (the wire bit is folded in on the read thread, with nothing
    // in the way); the observer is told only on the edge, so the mirror costs one relaxed load per
    // report. Fires on THIS thread, which is padMicMuteChanged's contract.
    void mirrorMicMute(const UsbReport& r) const {
        if (r.micMuted == lastMicMuted_->load(std::memory_order_relaxed)) { return; }
        lastMicMuted_->store(r.micMuted, std::memory_order_relaxed);
        if (observer_ != nullptr) { observer_->padMicMuteChanged(pad_, r.micMuted); }
    }

    // The charge the same way: the card shows it, AppModel's heartbeat puts the lower of it and
    // the host battery on the wire, and a level in tenths moves minutes apart.
    void mirrorBattery(const UsbReport& r) const {
        if (!r.batteryValid) { return; }
        const auto packed = static_cast<std::uint16_t>((r.batteryLevel << 8) | r.batteryStatus);
        if (packed == lastBattery_->load(std::memory_order_relaxed)) { return; }
        lastBattery_->store(packed, std::memory_order_relaxed);
        if (observer_ != nullptr) {
            observer_->padBatteryChanged(pad_, r.batteryLevel, r.batteryStatus);
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
    reducer::DirectPadKey pad_;

    // Per claim, and shared across the copies std::function makes so the edge is tracked in one
    // place. Mic mute starts unmuted, which is what a freshly claimed pad's latch says too, so the
    // first edge is a real press and not a startup echo.
    std::shared_ptr<std::atomic<bool>> lastMicMuted_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<std::uint16_t>> lastBattery_ =
        std::make_shared<std::atomic<std::uint16_t>>(kNoBatteryYet);
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
    for (auto& [syntheticId, claim] : claims_) {
        if (claim.worker != nullptr) { claim.worker->stop(); }
    }
}

std::shared_ptr<OutputWorker>
UsbGamepadManager::startWorker(int syntheticId, const UsbDeviceInfo& device,
                               const input::usbout::Ds5FirmwareInfo& ds5Firmware) {
    const auto parser = input::usbparse::parserForDevice(device.vendorId, device.productId);
    const bool isDualSense = parser == input::usbparse::HidParser::DualSense;
    const bool vibrationV2 =
        isDualSense && input::usbout::ds5UsesVibrationV2(device.productId, ds5Firmware);
    return std::make_shared<OutputWorker>(gateway_->openReportWriter(syntheticId), parser,
                                          vibrationV2);
}

// A model's own pad is on the slot list from its claim; a receiver's slot only once the gateway
// sees its controller (followPadLinks).
void UsbGamepadManager::recordPad(int lead, const UsbDeviceInfo& device,
                                  const ClaimResult& outcome) {
    const bool listed = !reducer::isReceiverSlot(padKeyOf(device));
    const PadClaim claim{device, lead,
                         startWorker(outcome.syntheticId, device, outcome.ds5Firmware), listed};
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        claims_[outcome.syntheticId] = claim;
    }
    if (listed) { listPad(outcome.syntheticId, device); }
}

void UsbGamepadManager::releasePad(int syntheticId) {
    std::shared_ptr<OutputWorker> worker;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        if (const auto it = claims_.find(syntheticId); it != claims_.end()) {
            worker = std::move(it->second.worker);
        }
    }
    if (worker != nullptr) { worker->stop(); }
    if (gateway_ != nullptr) { gateway_->releaseClaim(syntheticId); }
}

std::vector<int> UsbGamepadManager::padsOfClaim(int lead) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    std::vector<int> pads;
    for (const auto& [syntheticId, claim] : claims_) {
        if (claim.lead == lead) { pads.push_back(syntheticId); }
    }
    return pads;
}

void UsbGamepadManager::releasePads(int lead) {
    for (const int syntheticId : padsOfClaim(lead)) { releasePad(syntheticId); }
}

// The held placeholder goes with the claim, and every pad of it that was on the slot list leaves.
void UsbGamepadManager::dropPads(int lead) {
    for (const int syntheticId : padsOfClaim(lead)) {
        releasePad(syntheticId);
        if (forgetPad(syntheticId)) { unlistPad(syntheticId); }
    }
}

// True when the pad was on the slot list.
bool UsbGamepadManager::forgetPad(int syntheticId) {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claims_.find(syntheticId);
    if (it == claims_.end()) { return false; }
    const bool listed = it->second.linked;
    claims_.erase(it);
    return listed;
}

void UsbGamepadManager::listPad(int syntheticId, const UsbDeviceInfo& device) {
    if (observer_ == nullptr) { return; }
    observer_->syntheticAdded(
        syntheticId, device.name, device.hasImu,
        reducer::computeUsbPollRateHz(device.endpointInInterval, device.endpointInMaxPacket),
        device.vendorId, device.productId);
}

void UsbGamepadManager::unlistPad(int syntheticId) {
    if (observer_ != nullptr) { observer_->syntheticRemoved(syntheticId); }
}

// A receiver slot's pad joins the slot list with its controller and leaves with it.
void UsbGamepadManager::announcePad(int syntheticId, const PadClaim& claim) {
    if (claim.linked) {
        listPad(syntheticId, claim.device);
    } else {
        unlistPad(syntheticId);
    }
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
    for (const auto& pads : padsByModel(gateway_->enumerate())) {
        const UsbDeviceInfo& device = pads.front();
        presentKeys.insert(device.vpKey());
        if (claimEnded(device.vpKey())) { onUsbGone(device.vendorId, device.productId); }
        ensureTracked(pads);
        judgeClaim(device.vpKey());
    }
    sweepDeparted(presentKeys);
    followPadLinks();
}

void UsbGamepadManager::sweepDeparted(const std::unordered_set<int>& presentKeys) {
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

// A receiver's slot whose controller connected or disconnected since the last scan: the gateway
// saw it on the slot's wireless status event (or its first report), and the pad joins the slot list
// or leaves it here, on the thread that owns the list. The claim stays either way, so a controller
// that comes back reads again on its own slot.
void UsbGamepadManager::followPadLinks() {
    std::vector<std::pair<int, PadClaim>> moved;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        for (auto& [syntheticId, claim] : claims_) {
            const bool follows =
                reducer::isReceiverSlot(padKeyOf(claim.device)) && claim.worker != nullptr;
            if (!follows) { continue; }
            const bool linked = gateway_->isPadLinked(syntheticId);
            if (linked == claim.linked) { continue; }
            claim.linked = linked;
            moved.emplace_back(syntheticId, claim);
        }
    }
    for (const auto& [syntheticId, claim] : moved) { announcePad(syntheticId, claim); }
    if (!moved.empty() && observer_ != nullptr) { observer_->controllersChanged(); }
}

void UsbGamepadManager::ensureTracked(const std::vector<UsbDeviceInfo>& pads) {
    const UsbDeviceInfo& device = pads.front();
    const int key = device.vpKey();
    bool isNew = false;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        for (const UsbDeviceInfo& pad : pads) { devices_[padKeyOf(pad)] = pad; }
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
    const int lead = *c.syntheticId;
    return std::any_of(claims_.begin(), claims_.end(), [this, lead](const auto& entry) {
        return entry.second.lead == lead && !gateway_->isClaimAlive(entry.first);
    });
}

void UsbGamepadManager::judgeClaim(int key) {
    bool unreadable = false;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        const auto it = controllers_.find(key);
        if (it == controllers_.end()) {
            eraseModelPads(directScans_, key);
            return;
        }
        const reducer::UsbController& c = it->second;
        if (c.phase != reducer::UsbPhase::Direct || !c.syntheticId.has_value()) {
            eraseModelPads(directScans_, key);
            return;
        }
        const int lead = *c.syntheticId;
        for (const auto& [syntheticId, claim] : claims_) {
            if (claim.lead == lead && padReadsNothing(syntheticId, claim)) { unreadable = true; }
        }
    }
    if (unreadable) { applyEvent(key, reducer::event::ClaimUnreadable{}); }
}

// One pad's counts, under mtx_. A receiver's slot with no controller is not judged: its silence is
// the controller's absence, not a claim that reads nothing.
bool UsbGamepadManager::padReadsNothing(int syntheticId, const PadClaim& claim) {
    const reducer::DirectPadKey pad = padKeyOf(claim.device);
    if (!claim.linked) {
        directScans_.erase(pad);
        return false;
    }
    reducer::ClaimActivity activity;
    activity.reads = gateway_->completionCount(syntheticId);
    activity.decoded = gateway_->decodedCount(syntheticId);
    activity.scans = ++directScans_[pad];
    activity.streamsUnprompted = input::usbparse::parserStreamsUnprompted(
        input::usbparse::parserForDevice(pad.vendorId, pad.productId));
    return reducer::judgeClaimHealth(activity) == reducer::ClaimHealth::Unreadable;
}

void UsbGamepadManager::tryDirectMode(int vendorId, int productId) {
    setPathChoice(vendorId, productId, reducer::PathChoice::Direct);
}

void UsbGamepadManager::setPathChoice(int vendorId, int productId, reducer::PathChoice choice) {
    if (prefs_ != nullptr) { prefs_->setChoice(vendorId, productId, choice); }
    // If the device is present but untracked (chosen before the first scan),
    // bring it in first so the Choose lands on a tracked controller.
    trackPresent(vendorId, productId);
    applyEvent(vpKey(vendorId, productId), reducer::event::Choose{choice, /*userInitiated=*/true});
}

void UsbGamepadManager::clearChoice(int vendorId, int productId) {
    if (prefs_ != nullptr) { prefs_->clearChoice(vendorId, productId); }
    // Bring a present-but-untracked device in first (chosen before the first
    // scan), then re-resolve the path with the override now gone and drive it.
    trackPresent(vendorId, productId);
    applyEvent(vpKey(vendorId, productId),
               reducer::event::Choose{resolvePath(vendorId, productId), /*userInitiated=*/true});
}

void UsbGamepadManager::trackPresent(int vendorId, int productId) {
    if (gateway_ == nullptr) { return; }
    const int key = vpKey(vendorId, productId);
    for (const auto& pads : padsByModel(gateway_->enumerate())) {
        if (pads.front().vpKey() == key) { ensureTracked(pads); }
    }
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
    eraseModelPads(devices_, key);
    missedScans_.erase(key);
    eraseModelPads(directScans_, key);
    // A fresh plug-in of this model should re-evaluate Direct rather than inherit
    // a stale failure (android clears the registry's directFailed here).
    priorFailures_.erase(key);
}

void UsbGamepadManager::fireTimeout(int vendorId, int productId) {
    applyEvent(vpKey(vendorId, productId), reducer::event::Timeout{});
}

std::optional<UsbGamepadManager::DirectTarget>
UsbGamepadManager::directTarget(const reducer::DirectPadKey& pad) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = controllers_.find(modelKeyOf(pad));
    if (it == controllers_.end()) { return std::nullopt; }
    // Split from the lookup rather than folded into one `||`: a controller
    // being tracked and a controller holding a live claim are two different
    // facts, and separating them is also what lets the optional-access analysis
    // see that the dereference below is guarded.
    const reducer::UsbController& controller = it->second;
    if (!controller.syntheticId.has_value()) { return std::nullopt; }
    const int lead = *controller.syntheticId;
    const auto claim =
        std::find_if(claims_.begin(), claims_.end(), [lead, &pad](const auto& entry) {
            return entry.second.lead == lead && padKeyOf(entry.second.device) == pad;
        });
    if (claim == claims_.end()) { return std::nullopt; }
    DirectTarget t;
    t.vendorId = pad.vendorId;
    t.productId = pad.productId;
    t.syntheticId = claim->first;
    t.worker = claim->second.worker;
    // The family comes from the identity, not from the gateway: the manager
    // must answer the same question the capability solve asked when it built
    // the descriptor, and that one only ever had (vid, pid).
    t.parser = input::usbparse::parserForDevice(pad.vendorId, pad.productId);
    return t;
}

std::optional<UsbGamepadManager::DirectTarget>
UsbGamepadManager::directTarget(int vendorId, int productId) const {
    return directTarget(reducer::DirectPadKey{vendorId, productId});
}

bool UsbGamepadManager::isDirectClaimed(const reducer::DirectPadKey& pad) const {
    return directTarget(pad).has_value();
}

bool UsbGamepadManager::isDirectClaimed(int vendorId, int productId) const {
    return isDirectClaimed(reducer::DirectPadKey{vendorId, productId});
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

bool UsbGamepadManager::setPadMicMuted(const reducer::DirectPadKey& pad, bool muted) {
    const auto target = directTarget(pad);
    if (!target.has_value() || gateway_ == nullptr) { return false; }
    return gateway_->setPadMicMuted(target->syntheticId, muted);
}

bool UsbGamepadManager::setPadMicMuted(int vendorId, int productId, bool muted) {
    return setPadMicMuted(reducer::DirectPadKey{vendorId, productId}, muted);
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

std::vector<UsbGamepadManager::DirectPad> UsbGamepadManager::directPads() const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    std::vector<DirectPad> pads;
    for (const auto& [syntheticId, claim] : claims_) {
        if (claim.linked && leadsDirectClaim(claim)) {
            pads.push_back(DirectPad{padKeyOf(claim.device), syntheticId});
        }
    }
    std::sort(pads.begin(), pads.end(),
              [](const DirectPad& a, const DirectPad& b) { return a.key < b.key; });
    return pads;
}

// Under mtx_: the claim is the one its model holds on Direct, not a placeholder or a stale one.
bool UsbGamepadManager::leadsDirectClaim(const PadClaim& claim) const {
    const auto it = controllers_.find(claim.device.vpKey());
    if (it == controllers_.end()) { return false; }
    const reducer::UsbController& c = it->second;
    return c.phase == reducer::UsbPhase::Direct && c.syntheticId == claim.lead;
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
                if (c.syntheticId.has_value()) { releasePads(*c.syntheticId); }
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
                dropPads(e.syntheticId);
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

// In key order: a model's own pad, or each slot of a receiver.
std::vector<UsbDeviceInfo> UsbGamepadManager::trackedPads(int key) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    std::vector<UsbDeviceInfo> pads;
    for (const auto& [pad, device] : devices_) {
        if (modelKeyOf(pad) == key) { pads.push_back(device); }
    }
    return pads;
}

// Every pad of the model, or none: a receiver's slots share its one Direct pick, so a slot that
// will not open takes the claim with it, and the slots already open are let go. A device gone
// between Choose and Claim is Busy with the framework not stolen, android's `null -> Busy` arm.
ClaimResult UsbGamepadManager::claimPads(int key) {
    const std::vector<UsbDeviceInfo> pads = trackedPads(key);
    if (pads.empty()) { return ClaimResult::fail(reducer::DirectClaimFailure::Busy, false); }
    std::vector<std::pair<UsbDeviceInfo, ClaimResult>> opened;
    for (const UsbDeviceInfo& pad : pads) {
        const ClaimResult outcome = doClaim(pad);
        if (!outcome.ok) {
            letGo(opened);
            return outcome;
        }
        opened.emplace_back(pad, outcome);
    }
    const ClaimResult lead = opened.front().second;
    for (const auto& [pad, outcome] : opened) { recordPad(lead.syntheticId, pad, outcome); }
    return lead;
}

void UsbGamepadManager::letGo(const std::vector<std::pair<UsbDeviceInfo, ClaimResult>>& opened) {
    for (const auto& [pad, outcome] : opened) { gateway_->releaseClaim(outcome.syntheticId); }
}

void UsbGamepadManager::runClaim(int key) {
    const ClaimResult outcome = claimPads(key);
    if (outcome.ok) {
        applyEvent(key, reducer::event::ClaimSucceeded{outcome.syntheticId});
    } else {
        applyEvent(key, reducer::event::ClaimFailed{outcome.reason, outcome.frameworkStolen});
    }
}

void UsbGamepadManager::runReclaim(int key, const reducer::UsbController& c) {
    const std::optional<int> oldPlaceholder = c.syntheticId;
    const ClaimResult outcome = claimPads(key);
    // The old placeholder synthetic is dropped on a reclaim (android removes it
    // before feeding the outcome back).
    if (oldPlaceholder.has_value()) { dropPads(*oldPlaceholder); }
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
    // The input pipeline's device id is the pad's slot id (reducer::directSlotId).
    return gateway_->claim(device, ClaimedPadPublisher{processor_, observer_, padKeyOf(device)});
}

} // namespace dish::source::usb
