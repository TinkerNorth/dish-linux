// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include "core/input/HidTransport.h"
#include "core/input/UsbOutputReports.h"
#include "core/input/UsbReportParsers.h"
#include "JoystickMapping.h"
#include "PumpWaiter.h"
#include "SdlMotionConvert.h"
#include "Util/HostBattery.h"

#include <SDL2/SDL.h>

#include <QLoggingCategory>
#include <QMetaObject>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace dish::input {

namespace {

Q_LOGGING_CATEGORY(lcDishInput, "dish.input")

// Noise floor for every newly-attached controller: ~10 % of the int16 stick
// range and ~5 % of the 0..255 trigger range. Fixed values because SDL2 exposes
// no per-device flat/fuzz the way the Android input framework does.
constexpr std::int16_t kDefaultStickFlat = 3277;
constexpr std::uint8_t kDefaultTriggerFlat = 13;

// The satellite's own ViGEm pad enumerates as "Dish ...". Skipping it as an input
// stops a single-machine setup looping the emulated output back in. No real
// controller carries this name.
bool isDishVirtualDevice(const char* name) {
    return name != nullptr && QString::fromUtf8(name).startsWith(QStringLiteral("Dish "));
}

// SDL exposes no continuous battery level here: pads report only
// EMPTY/LOW/MEDIUM/FULL, and HID DualSense and some 8BitDo pads report UNKNOWN
// because their charging info rides separate hidraw paths. The four real
// readings become a coarse percent; WIRED and UNKNOWN carry no controller charge
// at all, so the host machine's battery is substituted rather than shipping a
// fake number.
using dish::util::BatteryReading;
using dish::util::kBatteryStatusDischarging;

BatteryReading powerLevelToWire(SDL_JoystickPowerLevel pl) {
    switch (pl) {
    case SDL_JOYSTICK_POWER_EMPTY:
        return {5, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_LOW:
        return {25, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_MEDIUM:
        return {60, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_FULL:
        return {100, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_WIRED:
    case SDL_JOYSTICK_POWER_UNKNOWN:
    default:
        return dish::util::readHostBattery();
    }
}

constexpr std::chrono::seconds kBatteryPollInterval{30};

QString padIdentity(SDL_Joystick* js) {
    char guid[64] = {0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof(guid));
    QString out = QStringLiteral("guid:") + QString::fromLatin1(guid);
    const char* serial = SDL_JoystickGetSerial(js);
    if (serial != nullptr && serial[0] != '\0') {
        out += QStringLiteral("/serial:") + QString::fromUtf8(serial);
    }
    return out;
}

} // namespace

SDLGamepadBridge::SDLGamepadBridge(GamepadInputProcessor* processor, QObject* parent)
    : QObject(parent), processor_(processor) {}

SDLGamepadBridge::~SDLGamepadBridge() { stop(); }

void SDLGamepadBridge::start(pump::PumpPolicy policy) {
    if (running_.exchange(true)) { return; }
    policy_ = policy;
    thread_ = std::thread([this] { runLoop(); });
}

void SDLGamepadBridge::stop() {
    if (!running_.exchange(false)) { return; }
    if (thread_.joinable()) { thread_.join(); }
}

QList<SDLGamepadBridge::Device> SDLGamepadBridge::devices() const {
    std::lock_guard<thread::TrackedMutex> lock(devicesMtx_);
    if (auto next = devicesBox_.take()) { devices_ = std::move(*next); }
    return devices_;
}

void SDLGamepadBridge::publishDevices() {
    std::vector<const Pad*> open;
    open.reserve(kMaxPads);
    for (const Pad& pad : pads_) {
        if (pad.kind != PadKind::Free) { open.push_back(&pad); }
    }
    std::sort(open.begin(), open.end(), [](const Pad* a, const Pad* b) { return a->iid < b->iid; });
    auto list = std::make_unique<QList<Device>>();
    list->reserve(static_cast<qsizetype>(open.size()));
    for (const Pad* pad : open) { list->append(pad->device); }
    devicesBox_.publish(std::move(list));
    QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
}

void SDLGamepadBridge::applySdlHints() {
    // Positional (Xbox-layout) buttons, not label-based, because the USB-direct
    // decoders map by physical position. Without this hint a Switch Pro would
    // disagree with itself across the SDL and Direct paths.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Qt owns this process's lifecycle. Left to itself SDL installs SIGINT and
    // SIGTERM handlers that raise SDL_QUIT, which nothing here consumes — so
    // the app ignored SIGTERM entirely and a logout sat through systemd's full
    // 90 s wait before the SIGKILL. Packaging CI is what caught it.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // A Sony pad on Bluetooth wakes in its simple report mode, where SDL's
    // HIDAPI driver sees buttons and sticks and nothing else: no rumble, no
    // lightbar, no gyro, no touchpad, no effects, and SDL reports none of them
    // as capabilities either. These hints open the pad in enhanced mode
    // instead, so a Bluetooth DualSense or DualShock 4 attaches with the same
    // surfaces as a wired one. The kernel's hid-playstation usually has the
    // pad there already, so this mostly costs nothing; where it does not, the
    // cost SDL documents is that the pad stays in that mode until it is
    // power-cycled.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
}

// False when SDL will not start at all, which leaves the bridge with no devices rather than a
// half-initialised one.
bool SDLGamepadBridge::initSdl() {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { return false; }
    SDL_GameControllerEventState(SDL_ENABLE);
    // Joystick event delivery is a separate opt-in even though SDL_INIT_JOYSTICK is already up.
    // Needed so generic pads SDL does not recognise as game controllers still surface.
    SDL_JoystickEventState(SDL_ENABLE);
    return true;
}

std::optional<std::size_t> SDLGamepadBridge::slotOf(int iid) const {
    for (std::size_t slot = 0; slot < pads_.size(); ++slot) {
        if (pads_[slot].kind != PadKind::Free && pads_[slot].iid == iid) { return slot; }
    }
    return std::nullopt;
}

SDLGamepadBridge::Pad* SDLGamepadBridge::padFor(int iid) {
    const auto slot = slotOf(iid);
    return slot.has_value() ? &pads_[*slot] : nullptr;
}

SDLGamepadBridge::Pad* SDLGamepadBridge::claimPad(int iid, PadKind kind) {
    for (Pad& pad : pads_) {
        if (pad.kind != PadKind::Free) { continue; }
        pad = Pad{};
        pad.iid = iid;
        pad.kind = kind;
        pad.id = "sdl:" + std::to_string(iid);
        pad.device.id = QString::fromStdString(pad.id);
        ++openPads_;
        return &pad;
    }
    return nullptr;
}

void SDLGamepadBridge::releasePad(Pad& pad) {
    if (!pad.id.empty()) { processor_->remove(pad.id); }
    pad = Pad{};
    --openPads_;
}

// Pushed from here rather than owned by the processor because the bridge is the only thing that
// knows when a device shows up; the processor learns the id before the GUI's device list does,
// so a user's dead zones set from that list land on the same record.
void SDLGamepadBridge::registerPad(Pad& pad) {
    pad.index = processor_->add(pad.id);
    // Feedback left for a device that held this index before is not this pad's.
    if (pad.index.has_value()) { mailboxes_[*pad.index].clear(); }
    processor_->setDeadzones(pad.id, {kDefaultStickFlat, kDefaultTriggerFlat});
    applyInputs(pad);
}

void SDLGamepadBridge::applyInputs(Pad& pad) const {
    pad.suppressed = inputs_->suppressed.count(pad.id) != 0;
    const auto it = inputs_->remaps.find({pad.device.vendorId, pad.device.productId});
    pad.remap = it != inputs_->remaps.end() ? it->second : JoystickRemap{};
}

void SDLGamepadBridge::adoptInputs() {
    auto next = inputsBox_.take();
    if (!next) { return; }
    inputs_ = std::move(next);
    for (Pad& pad : pads_) {
        if (pad.kind != PadKind::Free) { applyInputs(pad); }
    }
}

void SDLGamepadBridge::onControllerAdded(const SDL_Event& ev) {
    if (isDishVirtualDevice(SDL_GameControllerNameForIndex(ev.cdevice.which))) { return; }
    SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which);
    if (gc == nullptr) { return; }
    SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
    const int iid = SDL_JoystickInstanceID(js);
    Pad* const pad = claimPad(iid, PadKind::Controller);
    if (pad == nullptr) {
        qCWarning(lcDishInput) << "no room for another pad; closing" << iid;
        SDL_GameControllerClose(gc);
        return;
    }
    pad->gc = gc;
    pad->js = js;
    const auto* name = SDL_GameControllerName(gc);
    Device& d = pad->device;
    d.name = QString::fromUtf8(name != nullptr ? name : "Gamepad");
    // SDL's enable call returns success even for a device with no such sensor, so Has and Set
    // must both agree before a sensor is marked capable.
    const bool gyro = SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO) == SDL_TRUE &&
                      SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE) == 0;
    const bool accel = SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) == SDL_TRUE &&
                       SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE) == 0;
    d.motionCapable = gyro || accel;
    d.hasLightbar = SDL_GameControllerHasLED(gc) == SDL_TRUE;
    d.hasTouchpad = SDL_GameControllerGetNumTouchpads(gc) > 0;
    d.hasRumble = SDL_GameControllerHasRumble(gc) == SDL_TRUE;
    // SDL returns 0 when it cannot read the descriptor, which the twin-dedup pairing treats as
    // "no identity".
    d.vendorId = SDL_GameControllerGetVendor(gc);
    d.productId = SDL_GameControllerGetProduct(gc);
    // SDL's device path is the Win32 HID interface path for HIDAPI and RawInput pads, so the
    // marker check spots a Bluetooth link. A null path (the XInput fallback) reads as
    // not-Bluetooth, which fails safe to the wired presentation.
    const char* devPath = SDL_GameControllerPath(gc);
    d.bluetooth = devPath != nullptr && dish::input::isBluetoothHidDevicePath(devPath);
    d.identity = padIdentity(js);
    pad->shadow.seed(gc);

    // One-shot capability dump: type, ids and GUID together pin which mapping SDL applied, so a
    // "my pad doesn't work" report is diagnosable without a debugger.
    char guidBuf[64] = {0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
    qCInfo(lcDishInput) << "DEVCAPS id=" << d.id << "name=" << d.name
                        << "type=" << static_cast<int>(SDL_GameControllerGetType(gc))
                        << "vid=" << QString::number(d.vendorId, 16)
                        << "pid=" << QString::number(d.productId, 16) << "guid=" << guidBuf
                        << "gyro=" << gyro << "accel=" << accel << "led=" << d.hasLightbar
                        << "rumble=" << d.hasRumble << "bt=" << d.bluetooth;

    registerPad(*pad);
    publishDevices();
    rebuildState(*pad);
}

void SDLGamepadBridge::onControllerRemoved(const SDL_Event& ev) {
    Pad* const pad = padFor(ev.cdevice.which);
    if (pad == nullptr || pad->kind != PadKind::Controller) { return; }
    thread::blockingRegion("sdl.close");
    SDL_GameControllerClose(pad->gc);
    releasePad(*pad);
    publishDevices();
}

// One pass over what SDL knows about a raw joystick, in the order the tables need it; a split
// would put the DEVCAPS line on a different page from the reads it covers.
void SDLGamepadBridge::onJoystickAdded(const SDL_Event& ev) {
    // Unlike every other event here, this `which` is a device INDEX, not
    // an instance id — it matches SDL_JoystickOpen's argument.
    const int which = ev.jdevice.which;
    // A game controller ALSO emits joystick events, and the controller
    // path owns recognised pads. Opening one here would double it.
    if (SDL_IsGameController(which) == SDL_TRUE) { return; }
    if (isDishVirtualDevice(SDL_JoystickNameForIndex(which))) { return; }
    SDL_Joystick* js = SDL_JoystickOpen(which);
    if (js == nullptr) { return; }
    const int iid = SDL_JoystickInstanceID(js);
    Pad* const pad = claimPad(iid, PadKind::Joystick);
    if (pad == nullptr) {
        qCWarning(lcDishInput) << "no room for another pad; closing" << iid;
        SDL_JoystickClose(js);
        return;
    }
    pad->js = js;
    const auto* name = SDL_JoystickName(js);
    Device& d = pad->device;
    d.name = QString::fromUtf8(name != nullptr ? name : "Joystick");
    // SDL's joystick API exposes no IMU, LED or controller type, so a raw
    // joystick surfaces as a plain Xbox-kind pad with neither.
    d.vendorId = SDL_JoystickGetVendor(js);
    d.productId = SDL_JoystickGetProduct(js);
    const char* devPath = SDL_JoystickPath(js);
    d.bluetooth = devPath != nullptr && dish::input::isBluetoothHidDevicePath(devPath);
    d.hasRumble = SDL_JoystickHasRumble(js) == SDL_TRUE;
    d.isRawJoystick = true;
    d.identity = padIdentity(js);
    char guidBuf[64] = {0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
    qCInfo(lcDishInput) << "DEVCAPS(joystick) id=" << d.id << "name=" << d.name
                        << "axes=" << SDL_JoystickNumAxes(js)
                        << "buttons=" << SDL_JoystickNumButtons(js)
                        << "hats=" << SDL_JoystickNumHats(js)
                        << "vid=" << QString::number(d.vendorId, 16)
                        << "pid=" << QString::number(d.productId, 16) << "guid=" << guidBuf
                        << "bt=" << d.bluetooth;
    registerPad(*pad);
    publishDevices();
    rebuildJoystickState(*pad);
}

void SDLGamepadBridge::onJoystickRemoved(const SDL_Event& ev) {
    // Absent for a controller-path device, so this only acts on raw joysticks we actually opened.
    Pad* const pad = padFor(ev.jdevice.which);
    if (pad == nullptr || pad->kind != PadKind::Joystick) { return; }
    thread::blockingRegion("sdl.close");
    SDL_JoystickClose(pad->js);
    releasePad(*pad);
    publishDevices();
}

bool SDLGamepadBridge::isRawJoystick(int iid) const {
    const auto slot = slotOf(iid);
    return slot.has_value() && pads_[*slot].kind == PadKind::Joystick;
}

void SDLGamepadBridge::markDirty(int iid) {
    if (const auto slot = slotOf(iid); slot.has_value()) { dirty_.mark(*slot); }
}

// One report per moved pad per cycle, however many of its axes and buttons SDL reported.
void SDLGamepadBridge::rebuildDirty() {
    dirty_.drain([this](std::size_t slot) {
        Pad& pad = pads_[slot];
        if (pad.kind == PadKind::Controller) { rebuildState(pad); }
        if (pad.kind == PadKind::Joystick) { rebuildJoystickState(pad); }
    });
}

// One raw-joystick event. The tracked state always rebuilds; the capture page is offered the event
// only when it is listening AND the event was deliberate, which is the gate that keeps idle stick
// jitter and button releases from being read as an assignment. The four call sites differ only in
// what "deliberate" means for their kind.
void SDLGamepadBridge::onRawJoystickInput(int iid, CaptureKind kind, int index, int value,
                                          bool deliberate) {
    // A game controller's own joystick events land here too; its controller events carry the input.
    if (isRawJoystick(iid)) { markDirty(iid); }
    if (!deliberate) { return; }
    // Checked second: the flag keeps capture free when off, and it is read per event rather than
    // held, because the page can be left at any point in this loop.
    if (!captureEnabled_.load(std::memory_order_relaxed)) { return; }
    maybeEmitCapture(iid, static_cast<int>(kind), index, value);
}

// One arm per SDL event kind, each a forward. Long because SDL has this many kinds; a split
// would put a second name between an event and the handler already named for it.
void SDLGamepadBridge::dispatchSdlEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERDEVICEADDED:
        onControllerAdded(ev);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        onControllerRemoved(ev);
        break;
    case SDL_CONTROLLERDEVICEREMAPPED:
        if (Pad* pad = padFor(ev.cdevice.which); pad != nullptr && pad->gc != nullptr) {
            pad->shadow.seed(pad->gc);
            markDirty(pad->iid);
        }
        break;
    case SDL_JOYDEVICEADDED:
        onJoystickAdded(ev);
        break;
    case SDL_JOYDEVICEREMOVED:
        onJoystickRemoved(ev);
        break;
    case SDL_CONTROLLERAXISMOTION:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (Pad* pad = padFor(ev.cdevice.which); pad != nullptr && pad->shadow.apply(ev)) {
            markDirty(pad->iid);
        }
        break;
    case SDL_AUDIODEVICEADDED:
    case SDL_AUDIODEVICEREMOVED:
        // Delivered here because this loop is the process's one SDL event pump; the audio subsystem
        // itself is SdlAudioGateway's (see the signal's comment). A pad's own endpoints appear a
        // beat after its HID interface, so this edge is what re-runs the route matcher.
        QMetaObject::invokeMethod(this, "audioDevicesChanged", Qt::QueuedConnection);
        break;
    case SDL_JOYAXISMOTION:
        onRawJoystickInput(ev.jaxis.which, CaptureKind::Axis, ev.jaxis.axis, ev.jaxis.value,
                           captureAxisPasses(ev.jaxis.value));
        break;
    case SDL_JOYBUTTONDOWN:
        onRawJoystickInput(ev.jbutton.which, CaptureKind::Button, ev.jbutton.button, 1,
                           captureButtonPasses());
        break;
    case SDL_JOYBUTTONUP:
        // A release is not an assignment, so it rebuilds and offers nothing.
        onRawJoystickInput(ev.jbutton.which, CaptureKind::Button, ev.jbutton.button, 0,
                           /*deliberate=*/false);
        break;
    case SDL_JOYHATMOTION:
        onRawJoystickInput(ev.jhat.which, CaptureKind::Hat, ev.jhat.hat, ev.jhat.value,
                           captureHatPasses(ev.jhat.value));
        break;
    case SDL_CONTROLLERSENSORUPDATE:
        handleSensorEvent(ev.csensor);
        break;
    case SDL_CONTROLLERTOUCHPADDOWN:
    case SDL_CONTROLLERTOUCHPADMOTION:
    case SDL_CONTROLLERTOUCHPADUP:
        handleTouchpadEvent(ev.ctouchpad);
        break;
    default:
        break;
    }
}

// On the SDL thread, where every handle was opened: SDL's own documentation is explicit that a
// device must be closed from the thread that pumps its events.
void SDLGamepadBridge::closeAllDevices() {
    for (Pad& pad : pads_) {
        if (pad.kind == PadKind::Free) { continue; }
        thread::blockingRegion("sdl.close");
        if (pad.gc != nullptr) {
            SDL_GameControllerClose(pad.gc);
        } else if (pad.js != nullptr) {
            SDL_JoystickClose(pad.js);
        }
        releasePad(pad);
    }
    dirty_ = {};
    publishDevices();
}

void SDLGamepadBridge::runLoop() {
    const thread::ScopedThreadRole role(thread::ThreadRole::SdlInput);
    applySdlHints();
    if (!initSdl()) {
        running_.store(false);
        SDL_TLSCleanup();
        return;
    }

    // SDL_WaitEventTimeout with no window is a pump and a 1 ms sleep, which samples a pad every 1.5
    // to 2 ms; an open pad is pumped on the policy's period instead, from absolute deadlines.
    const PumpWaiter waiter(policy_.wait);
    pump::Schedule schedule(pump::Clock::now(), policy_.period);
    while (running_.load(std::memory_order_relaxed)) {
        if (openPads_ == 0) {
            adoptInputs();
            if (SDL_Event ev; SDL_WaitEventTimeout(&ev, kSdlWaitMs) != 0) { dispatchSdlEvent(ev); }
            schedule = pump::Schedule(pump::Clock::now(), policy_.period);
        } else {
            waiter.waitUntil(schedule.deadline());
        }
        pumpOnce(pump::Clock::now());
        schedule.advance(pump::Clock::now());
    }

    closeAllDevices();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    // SDL frees a thread's own storage (its error buffer) only on threads it started; this one is
    // a std::thread, so it hands that back itself.
    SDL_TLSCleanup();
}

void SDLGamepadBridge::pumpOnce(TimePoint now) {
    adoptInputs();
    SDL_PumpEvents();
    std::array<SDL_Event, kPeepBatch> events{};
    int got = 0;
    while ((got = SDL_PeepEvents(events.data(), kPeepBatch, SDL_GETEVENT, SDL_FIRSTEVENT,
                                 SDL_LASTEVENT)) > 0) {
        for (int i = 0; i < got; ++i) { dispatchSdlEvent(events[static_cast<std::size_t>(i)]); }
    }
    rebuildDirty();

    // Here, so every SDL_GameController* is resolved and used only on the SDL thread.
    drainMailboxes();

    if (now - lastBatterySweep_ >= kBatterySweepInterval) {
        lastBatterySweep_ = now;
        pollBatteries(now);
    }
}

void SDLGamepadBridge::postRumble(GamepadInputProcessor::DeviceIndex device,
                                  std::uint16_t strongMagnitude, std::uint16_t weakMagnitude,
                                  std::uint16_t durationMs) {
    if (device < mailboxes_.size()) {
        mailboxes_[device].postRumble(strongMagnitude, weakMagnitude, durationMs);
    }
}

void SDLGamepadBridge::setSuppressedDeviceIds(const std::unordered_set<std::string>& ids) {
    std::lock_guard<thread::TrackedMutex> lock(inputsMtx_);
    inputsDraft_.suppressed = ids;
    inputsBox_.publish(std::make_unique<const Inputs>(inputsDraft_));
}

void SDLGamepadBridge::setJoystickRemap(int vendorId, int productId, const JoystickRemap& remap) {
    std::lock_guard<thread::TrackedMutex> lock(inputsMtx_);
    inputsDraft_.remaps[{vendorId, productId}] = remap;
    inputsBox_.publish(std::make_unique<const Inputs>(inputsDraft_));
}

void SDLGamepadBridge::clearJoystickRemap(int vendorId, int productId) {
    std::lock_guard<thread::TrackedMutex> lock(inputsMtx_);
    inputsDraft_.remaps.erase({vendorId, productId});
    inputsBox_.publish(std::make_unique<const Inputs>(inputsDraft_));
}

void SDLGamepadBridge::setJoystickCaptureEnabled(bool enabled) {
    captureEnabled_.store(enabled, std::memory_order_relaxed);
}

void SDLGamepadBridge::maybeEmitCapture(int iid, int kind, int index, int value) {
    // Re-checked despite the callers' guard, so this is a safe no-op if ever
    // called unguarded.
    if (!captureEnabled_.load(std::memory_order_relaxed)) { return; }
    const Pad* const pad = padFor(iid);
    if (pad == nullptr) { return; }
    // QueuedConnection hops SDL thread to GUI thread; the args are value types,
    // so the cross-thread copy is safe.
    QMetaObject::invokeMethod(this, "rawJoystickInput", Qt::QueuedConnection,
                              Q_ARG(QString, pad->device.id), Q_ARG(int, kind), Q_ARG(int, index),
                              Q_ARG(int, value));
}

// Accel piggy-backs on the next gyro event so the rate limiter sees one stream, not two; a
// gyro-less pad therefore never drives MSG_MOTION, which is fine since it cannot do gyro aim
// anyway. No gyro sample goes out before an accel exists: publishing accel{0,0,0} would ship a
// spurious zero-gravity triple.
//
// Deliberately no rotation here: SDL applies the per-model matrix internally for HIDAPI
// controllers, so samples already arrive in the satellite's right-handed frame and only the units
// need converting.
void SDLGamepadBridge::handleSensorEvent(const SDL_ControllerSensorEvent& ev) {
    const bool isGyro = ev.sensor == SDL_SENSOR_GYRO;
    const bool isAccel = ev.sensor == SDL_SENSOR_ACCEL;
    if (!isGyro && !isAccel) { return; }
    Pad* const pad = padFor(ev.which);
    if (pad == nullptr || !pad->device.motionCapable) { return; }
    if (isAccel) {
        pad->accel = AccelCache{ev.data[0], ev.data[1], ev.data[2]};
        return;
    }
    // Twin-dedup: suppress motion too while USB-direct owns this pad. The accel above is still
    // cached, so the stream resumes on the first gyro sample after the claim is released.
    if (!pad->accel.has_value() || pad->suppressed || !pad->index.has_value()) { return; }

    GamepadInputProcessor::MotionSample sample{};
    sample.gyroX = gyroRadPerSecToInt16(ev.data[0]);
    sample.gyroY = gyroRadPerSecToInt16(ev.data[1]);
    sample.gyroZ = gyroRadPerSecToInt16(ev.data[2]);
    sample.accelX = accelMps2ToInt16(pad->accel->ax);
    sample.accelY = accelMps2ToInt16(pad->accel->ay);
    sample.accelZ = accelMps2ToInt16(pad->accel->az);
    processor_->publishMotion(*pad->index, sample);
}

// `finger` is the 0-based slot; the wire carries only two, so anything higher is dropped.
//
// Only a false-to-true DOWN edge takes a new tracking id, so a MOTION on an already-active finger
// keeps its id. The counter wraps freely: the protocol needs the id to CHANGE on a new contact,
// not to be globally unique.
void SDLGamepadBridge::applyTouchFinger(TouchState& ts, const SDL_ControllerTouchpadEvent& ev) {
    if (ev.finger < 0 || ev.finger >= 2) { return; }
    TouchFinger& f = ts.fingers[static_cast<std::size_t>(ev.finger)];
    if (ev.type == SDL_CONTROLLERTOUCHPADUP) {
        f.active = false;
        return;
    }
    if (ev.type == SDL_CONTROLLERTOUCHPADDOWN && !f.active) { ++f.id; }
    f.active = true;
    f.x = touchpadCoordToInt16(ev.x);
    f.y = touchpadCoordToInt16(ev.y);
}

void SDLGamepadBridge::handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev) {
    Pad* const pad = padFor(ev.which);
    if (pad == nullptr) { return; }
    applyTouchFinger(pad->touch, ev);
    // Twin-dedup: the touchpad surface is suppressed too while USB-direct owns the pad. The state
    // above is still tracked, so the surface resumes mid-gesture without a stale finger.
    if (pad->suppressed || !pad->index.has_value()) { return; }

    GamepadInputProcessor::TouchpadSample sample{};
    sample.finger0Active = pad->touch.fingers[0].active;
    sample.finger0Id = pad->touch.fingers[0].id;
    sample.finger0X = pad->touch.fingers[0].x;
    sample.finger0Y = pad->touch.fingers[0].y;
    sample.finger1Active = pad->touch.fingers[1].active;
    sample.finger1Id = pad->touch.fingers[1].id;
    sample.finger1X = pad->touch.fingers[1].x;
    sample.finger1Y = pad->touch.fingers[1].y;
    // The clickable-pad switch is an ordinary SDL button, which the shadow already holds.
    sample.buttonPressed = pad->shadow.pressed(SDL_CONTROLLER_BUTTON_TOUCHPAD);
    processor_->publishTouchpad(*pad->index, sample);
}

// Forwarded unconditionally: MSG_BATTERY is a 30 s heartbeat, so the receiver expects a packet
// each interval even when the value is unchanged, and a lost one self-heals on the next tick. A
// device polled for the first time is always due, so a pad that has just attached shows a charge
// without waiting out the interval. Only a moved value hands the GUI a new device list.
void SDLGamepadBridge::pollBatteries(TimePoint now) {
    bool anyChange = false;
    for (Pad& pad : pads_) {
        if (pad.kind == PadKind::Free || pad.js == nullptr) { continue; }
        const bool first = pad.lastBatteryPoll == TimePoint{};
        if (!first && (now - pad.lastBatteryPoll) < kBatteryPollInterval) { continue; }
        pad.lastBatteryPoll = now;
        const auto wire = powerLevelToWire(SDL_JoystickCurrentPowerLevel(pad.js));
        if (pad.index.has_value()) {
            processor_->publishBattery(
                *pad.index, GamepadInputProcessor::BatterySample{wire.level, wire.status});
        }
        if (pad.device.batteryLevel == wire.level && pad.device.batteryStatus == wire.status) {
            continue;
        }
        pad.device.batteryLevel = wire.level;
        pad.device.batteryStatus = wire.status;
        anyChange = true;
    }
    // One list per sweep, not per device, so the UI rebuilds once.
    if (anyChange) { publishDevices(); }
}

void SDLGamepadBridge::postLightbar(GamepadInputProcessor::DeviceIndex device, std::uint8_t r,
                                    std::uint8_t g, std::uint8_t b) {
    if (device < mailboxes_.size()) { mailboxes_[device].postLightbar(r, g, b); }
}

void SDLGamepadBridge::postTriggerEffects(
    GamepadInputProcessor::DeviceIndex device,
    const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& left,
    const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& right) {
    if (device < mailboxes_.size()) { mailboxes_[device].postTriggerEffects(left, right); }
}

void SDLGamepadBridge::postPlayerLeds(GamepadInputProcessor::DeviceIndex device,
                                      std::uint8_t ledMask) {
    if (device < mailboxes_.size()) { mailboxes_[device].postPlayerLeds(ledMask); }
}

void SDLGamepadBridge::postMicLed(GamepadInputProcessor::DeviceIndex device, std::uint8_t state) {
    if (device < mailboxes_.size()) { mailboxes_[device].postMicLed(state); }
}

void SDLGamepadBridge::drainMailboxes() {
    for (Pad& pad : pads_) {
        if (pad.kind != PadKind::Controller || !pad.index.has_value()) { continue; }
        if (const auto taken = mailboxes_[*pad.index].take()) { applyOutput(pad, *taken); }
    }
}

// Return values ignored: -1 means the pad has no such actuator or its driver changed under us, and
// neither the caller nor the satellite-side game has any recourse.
void SDLGamepadBridge::applyOutput(Pad& pad, const OutputMailbox::Taken& taken) {
    const OutputState& s = taken.state;
    if ((taken.kinds & output::kRumble) != 0) {
        SDL_GameControllerRumble(pad.gc, s.strong, s.weak, s.durationMs);
    }
    if ((taken.kinds & output::kLightbar) != 0) { SDL_GameControllerSetLED(pad.gc, s.r, s.g, s.b); }
    // The family from the pad's USB identity, which a Bluetooth pad reports too. The builders
    // answer 0 for any family without the surface, and the router only sends a DualSense here.
    const usbparse::HidParser parser =
        usbparse::parserForDevice(pad.device.vendorId, pad.device.productId);
    std::array<std::uint8_t, usbout::kMaxOutputReportBytes> report{};
    if ((taken.kinds & output::kTriggerEffects) != 0) {
        sendEffect(pad,
                   usbout::buildTriggerEffectsReport(parser, pad.effectState, s.leftTrigger.data(),
                                                     s.rightTrigger.data(), report.data(),
                                                     report.size()),
                   report);
    }
    if ((taken.kinds & output::kPlayerLeds) != 0) {
        // The Switch Pro's counter is unused: only the DualSense reaches here.
        sendEffect(pad,
                   usbout::buildPlayerLedsReport(parser, pad.effectState, s.ledMask, /*seq=*/0,
                                                 report.data(), report.size()),
                   report);
    }
    if ((taken.kinds & output::kMicLed) != 0) {
        sendEffect(pad,
                   usbout::buildMicMuteLedReport(parser, pad.effectState, s.micLedState,
                                                 report.data(), report.size()),
                   report);
    }
}

// The body without its report id: SDL's driver frames it for the link (see kDs5EffectBodyBytes).
void SDLGamepadBridge::sendEffect(
    Pad& pad, std::size_t reportBytes,
    const std::array<std::uint8_t, usbout::kMaxOutputReportBytes>& report) {
    if (reportBytes < usbout::kDs5EffectBodyOffset + usbout::kDs5EffectBodyBytes) { return; }
    SDL_GameControllerSendEffect(pad.gc, report.data() + usbout::kDs5EffectBodyOffset,
                                 static_cast<int>(usbout::kDs5EffectBodyBytes));
}

// A pad claimed by USB-direct streams over raw-HID only, so dropping here is what stops the
// satellite seeing it twice.
void SDLGamepadBridge::rebuildState(Pad& pad) {
    if (pad.suppressed || !pad.index.has_value()) { return; }
    processor_->publish(*pad.index, mapGamepad(pad.shadow.snapshot()));
}

void SDLGamepadBridge::rebuildJoystickState(Pad& pad) {
    if (pad.suppressed || !pad.index.has_value() || pad.js == nullptr) { return; }
    // Fixed caps keep the hot path allocation-free, and the buffers live on this frame so the
    // snapshot can borrow rather than own them.
    std::int16_t axes[kMaxJoystickAxes] = {0};
    bool buttons[kMaxJoystickButtons] = {false};
    std::uint8_t hats[kMaxJoystickHats] = {0};
    const JoystickSnapshot snap = readJoystick(pad.js, axes, buttons, hats);
    processor_->publish(*pad.index, mapJoystick(snap, pad.remap));
}

// Sized to the device's real counts, so the mapper's bounds checks see the true extent.
JoystickSnapshot SDLGamepadBridge::readJoystick(SDL_Joystick* js,
                                                std::int16_t (&axes)[kMaxJoystickAxes],
                                                bool (&buttons)[kMaxJoystickButtons],
                                                std::uint8_t (&hats)[kMaxJoystickHats]) {
    const int axisCount = std::min(SDL_JoystickNumAxes(js), kMaxJoystickAxes);
    const int buttonCount = std::min(SDL_JoystickNumButtons(js), kMaxJoystickButtons);
    const int hatCount = std::min(SDL_JoystickNumHats(js), kMaxJoystickHats);
    for (int i = 0; i < axisCount; ++i) { axes[i] = SDL_JoystickGetAxis(js, i); }
    for (int i = 0; i < buttonCount; ++i) { buttons[i] = SDL_JoystickGetButton(js, i) != 0; }
    for (int i = 0; i < hatCount; ++i) { hats[i] = SDL_JoystickGetHat(js, i); }

    JoystickSnapshot snap{};
    snap.axes = axes;
    snap.axisCount = axisCount;
    snap.buttons = buttons;
    snap.buttonCount = buttonCount;
    snap.hats = hats;
    snap.hatCount = hatCount;
    return snap;
}

} // namespace dish::input
