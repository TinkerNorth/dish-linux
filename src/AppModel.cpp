// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "AppModel.h"

#include "LightbarRouting.h"
#include "composer/StreamingSlotCount.h"
#include "core/input/UsbOutputReports.h"
#include "core/input/UsbReportParsers.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightTelemetry.h"
#include "core/moonlight/MoonlightTouchDiffer.h"
#include "core/reducer/CatalogPrewarm.h"
#include "core/reducer/PickerVisibility.h"
#include "core/reducer/RumbleRouting.h"
#include "core/reducer/SlotPathFields.h"
#include "core/reducer/TouchpadModeResolve.h"
#include "core/reducer/TouchpadRouting.h"
#include "core/reducer/UsbTwinDedup.h"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <QLocale>
#include <QStringList>

namespace dish {

namespace {

// The activity window's basis. Monotonic, so a wall-clock step cannot expire it.
std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Shared so both the SDL-slot and synthetic-slot rebuild arms thread through
// the same cross-reference.
void stampSlotPath(models::ControllerSlot& s, int vendorId, int productId, bool bluetooth,
                   const std::map<int, reducer::UsbController>& controllers) {
    const reducer::SlotPathFields f =
        reducer::slotPathFields(vendorId, productId, bluetooth, controllers);
    s.pathSupported = f.supported;
    s.pathPhase = f.phase;
    s.desiredPath = f.desired;
    s.directFailure = f.failure;
}

} // namespace

AppModel::AppModel(QObject* parent)
    : AppModel(std::make_unique<source::FreedesktopWakeInhibitor>(), parent) {}

AppModel::AppModel(std::unique_ptr<source::WakeInhibitor> inhibitor, QObject* parent)
    : QObject(parent), store_(std::make_unique<net::ConnectionStore>()),
      wifi_(new net::WifiConnectionManager(store_.get(), this)),
      hub_(new net::ConnectionHub(wifi_, store_.get(), this)),
      connections_(new composer::ConnectionCoordinator(wifi_, hub_, this)),
      bridge_(new input::SDLGamepadBridge(&processor_, this)),
      featureSettings_(new FeatureSettings(this)), autoReconnectTimer_(new QTimer(this)),
      inhibitor_(std::move(inhibitor)),
      controllerActivity_([this] { return processor_.actuationCount(); }),
      wakeComposer_(streamingSlotCount_, controllerActivity_.state(), keepAwakeStore_.state()),
      wakeController_(wakeComposer_.state(), inhibitor_.get()),
      themeController_(themeStore_.state()), crashController_(crashStore_.state(), &crashBackend_),
      tray_(source::makeSystemTrayIcon()),
      notifier_(std::make_unique<source::FreedesktopNotifier>()),
      sleepMonitor_(std::make_unique<source::LogindSleepMonitor>()),
      backgroundCoordinator_(new composer::BackgroundCoordinator(&backgroundStore_, tray_.get(),
                                                                 notifier_.get(), this)),
      sleepCoordinator_(new composer::SleepCoordinator(sleepMonitor_.get(), connections_, this)),
      trayComposer_(backgroundCoordinator_->windowVisible(), streamingSlotCount_),
      trayController_(trayComposer_.state(), tray_.get()), updateChecker_(&updatePrefs_, this),
      catalogHttp_(new net::HTTPClient(this)), catalogRepo_(catalogHttp_),
      motionEnabledStore_(&motionPrefRepo_), joystickRemapStore_(&joystickRemapRepo_),
      catalogSnapshot_(composer::CatalogSnapshot{}), catalogComposer_(catalogSnapshot_),
      usbPathStore_(&usbPathRepo_), usbObserver_(this), usbScanTimer_(new QTimer(this)),
      inputRateTimer_(new QTimer(this)),
      moonlight_(new source::moon::MoonlightManager(nullptr, this)) {
    QObject::connect(hub_, &net::ConnectionHub::changed, this, &AppModel::onHubChanged);
    QObject::connect(bridge_, &input::SDLGamepadBridge::devicesChanged, this,
                     &AppModel::onBridgeDevicesChanged);
    // An audio endpoint arriving or leaving re-runs the pad-to-endpoint
    // matcher: a composite pad's audio function enumerates a beat after its
    // HID interface, and an unplug must pull the caps back before the host
    // keeps streaming into nothing.
    QObject::connect(bridge_, &input::SDLGamepadBridge::audioDevicesChanged, this,
                     &AppModel::resolveAudioRoutes);
    // A direct relay: the bridge already QueuedConnection-hops to this thread.
    QObject::connect(bridge_, &input::SDLGamepadBridge::rawJoystickInput, this,
                     &AppModel::rawJoystickInput);
    QObject::connect(wifi_, &net::WifiConnectionManager::connectionEvent, this,
                     &AppModel::onWifiEvent);
    // ConnectionHub rolls the binding back on a rejected descriptor, so without
    // this the user watches their bind undo itself with no reason given.
    QObject::connect(wifi_, &net::WifiConnectionManager::slotRegistrationFailed, this,
                     [this](const QString&) {
                         emit errorMessage(
                             tr("The satellite wouldn’t accept that controller — binding undone."));
                     });
    // The toast for this already fires from onWifiEvent; this is the typed edge
    // the pairing sheet needs to stay open and mark the field inline.
    QObject::connect(wifi_, &net::WifiConnectionManager::pairingFailed, this,
                     &AppModel::pairingFailed);
    // poolChanged fires on every WifiConnection creation and state transition,
    // and installRumbleHandlers is idempotent over the already-wired ones.
    QObject::connect(wifi_, &net::WifiConnectionManager::poolChanged, this,
                     &AppModel::installRumbleHandlers);

    autoReconnectTimer_->setInterval(15'000);
    QObject::connect(autoReconnectTimer_, &QTimer::timeout, this,
                     [this] { wifi_->autoReconnectAll(); });

    // A rebuild whenever the Moonlight subsystem changes, so its rows and link
    // states reach the UI through the same stateChanged() the satellite pool
    // uses. The manager already coalesces on the Qt main thread.
    QObject::connect(moonlight_, &source::moon::MoonlightManager::rowsChanged, this,
                     &AppModel::stateChanged);
    // A binding is durable intent: one made against a host that was not paired
    // yet is recorded and waits. This is what stops the wait. Re-binding the
    // slot is a restart, not a second pad, and it is the same call the binding
    // flow makes, so the hot-path tables are wired the way a fresh bind wires
    // them.
    QObject::connect(moonlight_, &source::moon::MoonlightManager::pairingFinished, this,
                     [this](const QString& uuid, bool ok, const QString&) {
                         if (!ok) { return; }
                         for (const auto& slotId : moonlight_->boundSlots(uuid)) {
                             bindMoonlightSlot(slotId, uuid);
                         }
                     });
    QObject::connect(moonlight_, &source::moon::MoonlightManager::sessionFailed, this,
                     [this](const QString&, const QString& reasonToken) {
                         if (reasonToken == QLatin1String("appAlreadyRunning")) {
                             emit errorMessage(
                                 tr("That host is already running an app. Stop it on the host, "
                                    "then try again."));
                             return;
                         }
                         emit errorMessage(tr("The Moonlight session ended."));
                     });
    // Host->local actuation shares the SDL output plumbing the satellite path
    // uses. The manager has already resolved the event's controller number to
    // the pad that holds it, because one session drives up to four.
    moonlight_->setRumbleSink(
        [this](const QString& slotId, std::uint16_t strong, std::uint16_t weak) {
            if (slotId.isEmpty()) { return; }
            // Straight through: the session has already mapped the wire's
            // low/high onto the pad's motors and mixed the host's trigger
            // stream in, so this is the final value for both. Duration 0
            // because the host refreshes on its own schedule and a stop is an
            // explicit 0,0.
            actuateRumble(slotId, strong, weak, 0);
        });
    moonlight_->setLedSink(
        [this](const QString& slotId, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            if (slotId.isEmpty()) { return; }
            actuateLightbar(slotId, r, g, b);
        });

    // Hot path, called on the SDL gamepad thread: look the sender up under a
    // short-held mutex, then forward outside it.
    processor_.setReportSender([this](const std::string& did, std::uint16_t buttons,
                                      std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                                      std::int16_t ly, std::int16_t rx, std::int16_t ry) {
        net::ConnectionHub::ReportSender sender;
        {
            std::lock_guard<std::mutex> lock(routingMtx_);
            const QString key = QString::fromStdString(did);
            sender = routing_.value(key);
            // A slot bound to a Moonlight host has no satellite sender; fall
            // through to the Moonlight table. The two are mutually exclusive.
            if (!sender) { sender = moonlightRouting_.value(key); }
        }
        if (sender) { sender(buttons, lt, rt, lx, ly, rx, ry); }
    });

    processor_.setMotionSender([this](const std::string& did, std::int16_t gx, std::int16_t gy,
                                      std::int16_t gz, std::int16_t ax, std::int16_t ay,
                                      std::int16_t az, std::uint32_t dtUs) {
        net::ConnectionHub::MotionSender sender;
        {
            std::lock_guard<std::mutex> lock(routingMtx_);
            const QString key = QString::fromStdString(did);
            sender = motionRouting_.value(key);
            if (!sender) { sender = moonlightMotionRouting_.value(key); }
        }
        if (sender) { sender(gx, gy, gz, ax, ay, az, dtUs); }
    });

    processor_.setBatterySender(
        [this](const std::string& did, std::uint8_t level, std::uint8_t status) {
            net::ConnectionHub::BatterySender sender;
            {
                std::lock_guard<std::mutex> lock(routingMtx_);
                const QString key = QString::fromStdString(did);
                sender = batteryRouting_.value(key);
                // Same fall-through as the report and motion senders: a slot is
                // routed to one transport, and a Moonlight host that declared
                // CAP_BATTERY is waiting for exactly this.
                if (!sender) { sender = moonlightBatteryRouting_.value(key); }
            }
            if (sender) { sender(level, status); }
        });

    processor_.setTouchpadSender([this](const std::string& did,
                                        const input::GamepadInputProcessor::TouchpadSample& s) {
        net::ConnectionHub::TouchpadSender sender;
        {
            std::lock_guard<std::mutex> lock(routingMtx_);
            const QString key = QString::fromStdString(did);
            sender = touchpadRouting_.value(key);
            // A Moonlight-bound slot takes the SAME full-state frame; its sender
            // diffs it into CONTROLLER_TOUCH events on the way out.
            if (!sender) { sender = moonlightTouchRouting_.value(key); }
        }
        if (sender) {
            // MSG_TOUCHPAD carries a sender-side uptime-ms stamp, because
            // mouse-mode timing scales by the delta between consecutive
            // samples. This path forwards every assembled state change with no
            // resends, so a fresh monotonic stamp per publish is the contract's
            // eventTimeMs.
            const auto nowMs =
                static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
            const auto fwd = reducer::assembleTouchpadForward(
                s.finger0Active, s.finger0Id, s.finger0X, s.finger0Y, s.finger1Active, s.finger1Id,
                s.finger1X, s.finger1Y, s.buttonPressed, nowMs);
            sender(fwd.finger0Active, fwd.finger0Id, fwd.finger0X, fwd.finger0Y, fwd.finger1Active,
                   fwd.finger1Id, fwd.finger1X, fwd.finger1Y, fwd.buttonPressed, fwd.eventTimeMs);
        }
    });

    // Every actuator cap goes through the one router, so the descriptor claims
    // exactly what actuate*() would land: the SDL probe for a framework slot,
    // the parser family plus a live claim for a synthetic one.
    hub_->setLightbarCapabilityFn([this](const QString& slotId) {
        return reducer::slotCarriesFeedback(feedbackInputs(slotId),
                                            reducer::FeedbackKind::Lightbar);
    });

    // Protocol 2. Direct-path only by construction: the router knows SDL has no
    // call for either, so a Standard slot never advertises them and the
    // satellite never sends 0x0010 / 0x0011 into a path that would drop them.
    hub_->setTriggerEffectsCapabilityFn([this](const QString& slotId) {
        return reducer::slotCarriesFeedback(feedbackInputs(slotId),
                                            reducer::FeedbackKind::TriggerEffects);
    });
    hub_->setPlayerLedsCapabilityFn([this](const QString& slotId) {
        return reducer::slotCarriesFeedback(feedbackInputs(slotId),
                                            reducer::FeedbackKind::PlayerLeds);
    });

    // CAP_MOTION is sent iff the pad HAS a gyro and the user left motion
    // enabled, so an Xbox pad never advertises it. Same rule as
    // MotionCapability::toCapBits. A synthetic slot reads the parser family, so
    // a Direct-claimed DualSense/DS4/Switch Pro advertises the IMU its decoder
    // streams (a bridge-only scan used to miss it, and the satellite dropped
    // every MOTION packet a Direct pad sent).
    hub_->setMotionCapabilityFn([this](const QString& slotId) {
        return slotHardware(slotId).hasMotion &&
               motionEnabledStore_.isEnabled(slotId.toStdString());
    });

    // CAP_RUMBLE follows the active path's real actuator: the SDL probe for a
    // Standard slot, the claim's OUT report path for a Direct one, so the
    // satellite only offers rumble where it actually fires.
    hub_->setRumbleCapabilityFn([this](const QString& slotId) {
        return reducer::slotCarriesFeedback(feedbackInputs(slotId), reducer::FeedbackKind::Rumble);
    });

    // CAP_MIC / CAP_SPEAKER: the audio route (a Wave-2 seam that answers false
    // for every slot today) gated by the per-binding toggle, same shape as
    // CAP_MOTION above. The mic gate matters doubly — CAP_MIC is also what
    // invites the MIC_LED return path, and a lamp for a microphone the user
    // switched off would report a stream that cannot exist.
    hub_->setMicCapabilityFn([this](const QString& slotId) {
        return slotCarriesMicSource(slotId) && micEnabledStore_.isEnabled(slotId.toStdString());
    });
    hub_->setSpeakerCapabilityFn([this](const QString& slotId) {
        return slotCarriesSpeakerSink(slotId) &&
               speakerEnabledStore_.isEnabled(slotId.toStdString());
    });

    // The user's Emulate override wins over the SDL hardware classification;
    // resolveControllerType applies that ladder.
    hub_->setControllerTypeFn(
        [this](const QString& slotId) { return resolveControllerType(slotId); });

    // Declares ds4 pad-render when the pad has a touch source and the resolved
    // type is DS4. The per-satellite pick defaults to ds4, so DS4 touch forwards
    // out of the box; mouse stays unreachable while no UI sets the pick to
    // "mouse" and hostMouseControl reads false. Answering "off" here would make
    // the satellite discard every MSG_TOUCHPAD the forward path sends.
    hub_->setTouchpadModeFn([this](const QString& slotId) -> std::uint8_t {
        // slotHardware covers the synthetic ids too: a Direct-claimed DS4 /
        // DualSense decodes the touch block itself, so its descriptor must
        // declare the render mode or the satellite discards the forward.
        const bool hasTouchpad = slotHardware(slotId).hasTouchpad;
        if (!hasTouchpad) { return proto::kTouchpadModeOff; }
        const auto connId = hub_->boundConnection(slotId);
        const std::string pick =
            connId.has_value()
                ? touchpadModeStore_.modeFor(connId->id.toStdString())
                      .value_or(std::string(proto::touchpadModeName(proto::kTouchpadModeDs4)))
                : std::string(proto::touchpadModeName(proto::kTouchpadModeDs4));
        // kControllerTypePlayStation is the one type whose catalog touchpad
        // feature carries the "ds4" mode in every catalog the contract pins, so
        // this stands in for a real per-satellite CatalogFeatureGate lookup.
        const bool typeOffersDs4 =
            resolveControllerType(slotId) == proto::kControllerTypePlayStation;
        return reducer::resolveTouchpadMode(pick, hasTouchpad, typeOffersDs4,
                                            /*hostMouseControl=*/false);
    });

    // Dormant until start() arms the scan timer. reconcile() then auto-claims
    // the verified fast-lane models; every other or failed pad stays on SDL via
    // the FSM's Routed phase.
    usbGateway_ = std::make_unique<source::usb::HidrawGateway>();
    usbManager_ = std::make_unique<source::usb::UsbGamepadManager>(usbGateway_.get(), &processor_,
                                                                   &usbPathStore_, &usbObserver_);
    // Enumeration only, never per-report, and on the main thread because that is
    // the only thread that may mutate the FSM.
    usbScanTimer_->setInterval(1000);
    QObject::connect(usbScanTimer_, &QTimer::timeout, this, &AppModel::pollUsbDirect);

    // emitCurrent=false because the explicit push below covers the initial
    // state; the subscription handles only subsequent republishes.
    joystickRemapSub_ = joystickRemapStore_.state().subscribe(
        [this](const source::JoystickRemapMap&) { pushJoystickRemapsToBridge(); },
        /*emitCurrent=*/false);
    pushJoystickRemapsToBridge();

    controllerActivity_.setIdleTimeoutMinutes(keepAwakeStore_.idleTimeoutMinutes());
    keepAwakePrefsSub_ = keepAwakeStore_.state().subscribe(
        [this](const reducer::KeepAwakePreferences& prefs) {
            controllerActivity_.setIdleTimeoutMinutes(prefs.idleTimeoutMinutes);
        },
        /*emitCurrent=*/false);

    // Each start() applies its current value immediately, so the persisted wake
    // intent, palette and crash opt-in all take effect without waiting for a
    // first change.
    wakeController_.start();
    themeController_.start();
    crashController_.start();

    // The CounterSource borrows processor_, which outlives it. slotId is the
    // SDL device id or USB-direct synthetic key — exactly the key
    // processor_.publish() counts under, so no translation is needed.
    inputRateStore_ = std::make_unique<source::InputRateStore>(
        [this](const std::string& slotId) -> source::SlotInputCounters {
            const auto c = processor_.inputCounters(slotId);
            return source::SlotInputCounters{c.gamepadEvents, c.motionEvents};
        });
    inputRatesSub_ = inputRateStore_->state().subscribe(
        [this](const source::SlotInputRatesMap& rates) { onInputRatesChanged(rates); },
        /*emitCurrent=*/false);
    // sampleAt() is pumped with the steady clock rather than the store's own
    // QTimer, so sampling stays on the AppModel's thread alongside the slot-list
    // state it patches.
    inputRateTimer_->setInterval(1000);
    QObject::connect(inputRateTimer_, &QTimer::timeout, this, [this] {
        const auto nowUs =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
        inputRateStore_->sampleAt(nowUs);
        controllerActivity_.sampleAt(steadyNowMs());
    });

    rebuild();
}

AppModel::~AppModel() {
    // Order matters. Stop the input thread first so no further SDL report races
    // teardown; the timers next so no queued pollUsbDirect fires against a
    // half-torn-down manager. The gateway's destructor is what releases the live
    // claims, so it must outlive the manager that borrows it.
    bridge_->stop();
    usbScanTimer_->stop();
    inputRateTimer_->stop();
    // Unregister the tray item and drop the suspend delay lock explicitly: a
    // panel that outlives us must not keep drawing a dead item, and a leaked
    // delay lock makes the next suspend wait out logind's timeout.
    trayController_.stop();
    sleepMonitor_->stop();
    // Drop each subscription before its store, so no late emission races
    // teardown and pushes into a half-gone bridge.
    keepAwakePrefsSub_ = arch::Observable<reducer::KeepAwakePreferences>::Subscription{};
    inputRatesSub_ = arch::Observable<source::SlotInputRatesMap>::Subscription{};
    inputRateStore_.reset();
    joystickRemapSub_ = arch::Observable<source::JoystickRemapMap>::Subscription{};
    usbManager_.reset();
    usbGateway_.reset();
}

void AppModel::installRumbleHandlers() {
    for (auto* conn : wifi_->connections()) {
        const QString id = conn->id();
        if (rumbleWiredConnections_.contains(id)) { continue; }
        rumbleWiredConnections_.insert(id);
        // The handler runs on the SatelliteClient receive thread, so it only
        // reads structures protected by their own locks.
        conn->setRumbleHandler([this, id](const net::SatelliteClient::RumbleMessage& rm) {
            // Snapshot the bindings ONCE, then decide with a pure function, so
            // the receive thread never reads a half-updated table. slot.id is
            // the SDL bridge device id, so the resolved target is what to
            // actuate.
            const auto bindings = hub_->bindings();
            std::vector<reducer::RumbleConnectionSnapshot> snapshot;
            for (auto* c : wifi_->connections()) {
                reducer::RumbleConnectionSnapshot s;
                s.connId = c->id();
                s.connected = c->state() == net::SessionState::Live;
                for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
                    if (it.value() == s.connId) {
                        s.boundDeviceId = it.key();
                        break;
                    }
                }
                snapshot.push_back(std::move(s));
            }
            const auto target = reducer::resolveRumble(snapshot, id);
            if (!target.valid()) { return; }
            // Vibration only: the light bar has its own return path via
            // MSG_LIGHTBAR. actuateRumble picks the path the slot is actually
            // on, so a Direct-claimed pad rumbles over its own OUT endpoint.
            actuateRumble(target.deviceId, rm.strongMagnitude, rm.weakMagnitude, rm.durationMs);
        });
        // The MSG_LIGHTBAR stream, independent of rumble. Gated by the light-bar
        // setting: "Off" suppresses the colour entirely.
        conn->setLightbarHandler([this, id](const net::SatelliteClient::LightbarMessage& lm) {
            const auto color =
                lightbarColorFromLightbarMessage(lm, featureSettings_->lightbarFollowGame());
            if (!color) { return; }
            const QString deviceId = boundSlotForConnection(id);
            if (deviceId.isEmpty()) { return; }
            actuateLightbar(deviceId, color->r, color->g, color->b);
        });
        // MSG_TRIGGER_EFFECTS: the game's own DualSense effect blocks, replayed
        // verbatim. Arrives only for a slot whose descriptor advertised the
        // actuator, so there is nothing to gate here beyond finding the slot.
        conn->setTriggerEffectsHandler(
            [this, id](const net::SatelliteClient::TriggerEffectsMessage& tm) {
                const QString deviceId = boundSlotForConnection(id);
                if (deviceId.isEmpty()) { return; }
                actuateTriggerEffects(deviceId, tm.left, tm.right);
            });
        // MSG_PLAYER_LEDS: the indicator bar. Deliberately NOT gated on the
        // light-bar setting — that switch is about the RGB colour following the
        // game, and a player number is not a colour.
        conn->setPlayerLedsHandler([this, id](const net::SatelliteClient::PlayerLedsMessage& pm) {
            const QString deviceId = boundSlotForConnection(id);
            if (deviceId.isEmpty()) { return; }
            actuatePlayerLeds(deviceId, pm.ledMask);
        });
        // MSG_SPEAKER_AUDIO: straight into the playout engine on the receive
        // thread — reorder window, Opus decode, the pad's own endpoint. The
        // message borrows the receive buffer and the engine consumes it before
        // returning, which is the SpeakerAudioMessage contract. Arrives only
        // for a slot whose descriptor claimed CAP_SPEAKER; a frame for a voice
        // the engine does not hold is dropped inside deliver().
        conn->setSpeakerAudioHandler(
            [this, id](const net::SatelliteClient::SpeakerAudioMessage& sm) {
                speakerEngine_.deliver(id.toStdString(), sm.controllerIndex, sm.seq, sm.opus,
                                       sm.opusLen);
            });
        // MSG_MIC_LED: the mute lamp, routed like every other feedback kind.
        conn->setMicLedHandler([this, id](const net::SatelliteClient::MicLedMessage& mm) {
            const QString deviceId = boundSlotForConnection(id);
            if (deviceId.isEmpty()) { return; }
            actuateMicLed(deviceId, mm.state);
        });
    }
}

QString AppModel::boundSlotForConnection(const QString& connectionId) const {
    const auto bindings = hub_->bindings();
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        if (it.value() == connectionId) { return it.key(); }
    }
    return {};
}

void AppModel::bindMoonlightSlot(const QString& slotId, const QString& hostUuid) {
    if (hostUuid.isEmpty()) {
        unbindMoonlightSlot(slotId);
        return;
    }
    // What the pad itself can deliver. The declared CONTROLLER_ARRIVAL bitfield
    // is this intersected with the emulated type's ceiling, because declaring a
    // capability the source cannot provide makes the host ask for reports that
    // never arrive.
    const SlotHardware hardware = slotHardware(slotId);
    moonlight::SourceCapabilities source;
    source.rumble = hardware.hasRumble;
    source.motion = hardware.hasMotion;
    source.touchpad = hardware.hasTouchpad;
    source.lightbar = hardware.hasLightbar;
    // A pad reporting any level at all has a battery to report. Left false
    // before, which meant CAP_BATTERY was never declared and the host had no
    // reason to read the level the pad was already publishing.
    source.battery = slotBatteryLevel(slotId) != net::SatelliteClient::kBatteryLevelUnknown;
    // The type is a property of the BINDING, so it comes from the per-slot
    // override the binding flow writes; Auto resolves against the pad above.
    const int storedType = typeStore_.typeFor(hostUuid.toStdString(), slotId.toStdString())
                               .value_or(repository::kMoonlightControllerTypeAuto);

    const auto number = moonlight_->bindController(slotId, hostUuid, storedType, source);

    net::ConnectionHub::ReportSender reportSender;
    net::ConnectionHub::MotionSender motionSender;
    net::ConnectionHub::BatterySender batterySender;
    net::ConnectionHub::TouchpadSender touchSender;
    if (number) {
        // Raw pointers: the session is parented to the manager, which outlives
        // the SDL thread (stopped in ~AppModel before these tables clear). The
        // controller number is resolved once here so the hot path carries it.
        auto* session = moonlight_->session(hostUuid);
        const std::uint8_t pad = *number;
        if (session != nullptr) {
            reportSender = [session, pad](std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt,
                                          std::int16_t lx, std::int16_t ly, std::int16_t rx,
                                          std::int16_t ry) {
                session->sendControllerState(pad, buttons, lt, rt, lx, ly, rx, ry);
            };
            motionSender = [session, pad](std::int16_t gx, std::int16_t gy, std::int16_t gz,
                                          std::int16_t ax, std::int16_t ay, std::int16_t az,
                                          std::uint32_t) {
                // Gyro sample first, then accel; the host asks for whichever it
                // wants via MOTION_EVENT, and sendMotion no-ops until then.
                //
                // The sources hand over the SATELLITE's fixed-point scaling and
                // this wire wants physical units, so the conversion is not
                // optional: forwarding the raw int16 as a float reports a pad at
                // rest correctly and a moving one at tens of thousands of
                // degrees per second.
                session->sendMotion(pad, moonproto::kMotionGyroscope, moonlight::gyroDegS(gx),
                                    moonlight::gyroDegS(gy), moonlight::gyroDegS(gz));
                session->sendMotion(pad, moonproto::kMotionAcceleration, moonlight::accelMs2(ax),
                                    moonlight::accelMs2(ay), moonlight::accelMs2(az));
            };
            batterySender = [session, pad](std::uint8_t level, std::uint8_t status) {
                session->sendBattery(pad, moonlight::batteryStateFromSatelliteStatus(status),
                                     moonlight::batteryPercentage(level));
            };
            // Per bound pad, because the differ IS the pad's last frame: the
            // host wants transitions and the pad reports full state.
            auto differ = std::make_shared<moonlight::MoonlightTouchDiffer>();
            touchSender = [session, pad, differ](
                              bool f0Active, std::uint8_t f0Id, std::int16_t f0X, std::int16_t f0Y,
                              bool f1Active, std::uint8_t f1Id, std::int16_t f1X, std::int16_t f1Y,
                              bool /*buttonPressed*/, std::uint32_t /*eventTimeMs*/) {
                // The pad click has no packet of its own here: it rides the pad
                // frame as BTN_TOUCHPAD, which the report sender already
                // carries, so only the finger positions come this way.
                moonlight::TouchFinger a;
                a.active = f0Active;
                a.id = f0Id;
                a.x = moonlight::touchNorm(f0X);
                a.y = moonlight::touchNorm(f0Y);
                moonlight::TouchFinger b;
                b.active = f1Active;
                b.id = f1Id;
                b.x = moonlight::touchNorm(f1X);
                b.y = moonlight::touchNorm(f1Y);
                for (const auto& event : differ->diff(a, b)) { session->sendTouch(pad, event); }
            };
        }
    }
    {
        std::lock_guard<std::mutex> lock(routingMtx_);
        // A slot routes to exactly one transport: clear any satellite route so
        // the report sender's fall-through reaches the Moonlight table.
        routing_.remove(slotId);
        motionRouting_.remove(slotId);
        batteryRouting_.remove(slotId);
        touchpadRouting_.remove(slotId);
        if (reportSender) {
            // operator[], not insert: QHash::insert takes the value by const
            // reference, so a std::move into it would silently copy the
            // std::function (and its captured state) instead of moving.
            moonlightRouting_[slotId] = std::move(reportSender);
            moonlightMotionRouting_[slotId] = std::move(motionSender);
            moonlightBatteryRouting_[slotId] = std::move(batterySender);
            moonlightTouchRouting_[slotId] = std::move(touchSender);
        } else {
            moonlightRouting_.remove(slotId);
            moonlightMotionRouting_.remove(slotId);
            moonlightBatteryRouting_.remove(slotId);
            moonlightTouchRouting_.remove(slotId);
        }
    }
    // rebuild() re-derives the slot list against the new binding and emits.
    rebuild();
}

void AppModel::unbindMoonlightSlot(const QString& slotId) {
    {
        std::lock_guard<std::mutex> lock(routingMtx_);
        moonlightRouting_.remove(slotId);
        moonlightMotionRouting_.remove(slotId);
        moonlightBatteryRouting_.remove(slotId);
        moonlightTouchRouting_.remove(slotId);
    }
    // Drops this pad from the host's shared session, and tears the session down
    // behind the last one so the app is not stranded.
    moonlight_->unbindController(slotId);
    rebuild();
}

QString AppModel::moonlightBoundHostFor(const QString& slotId) const {
    return moonlight_->boundHostFor(slotId);
}

void AppModel::forgetMoonlightHost(const QString& hostUuid) {
    if (hostUuid.isEmpty()) { return; }
    // UNBIND BEFORE FORGETTING, the order ConnectionCoordinator uses for a
    // satellite. moonlightRouting_ holds lambdas that captured the session by
    // raw pointer and is read on the SDL gamepad thread, so a route still in
    // the table when forget() deletes that session is a use after free there.
    // unbindMoonlightSlot also sends each pad its farewell CONTROLLER_MULTI and
    // hands the app back behind the last one, instead of dropping the session.
    for (const auto& slotId : moonlight_->boundSlots(hostUuid)) { unbindMoonlightSlot(slotId); }
    // The Emulate overrides are keyed (connection, slot), and the connection
    // here IS the host. Left behind, they would seed a later re-pairing of the
    // same box with picks made for a trust relationship that no longer exists.
    typeStore_.clearConnection(hostUuid.toStdString());
    moonlight_->forget(hostUuid);
    rebuild();
}

std::optional<models::ConnectionSummary> AppModel::moonlightSummary(const QString& uuid) const {
    const auto row = moonlight_->row(uuid);
    if (!row) { return std::nullopt; }
    models::ConnectionSummary summary;
    summary.id = row->uuid;
    summary.label = row->name;
    summary.detail = row->address;
    // Saved, not Disconnected, for a host that is merely not streaming: there
    // is no link to have lost. Only a live control stream reads Connected.
    switch (row->link) {
    case source::moon::MoonlightLinkState::Live:
        summary.live = models::LinkState::Connected;
        break;
    case source::moon::MoonlightLinkState::Linking:
        summary.live = models::LinkState::Connecting;
        break;
    case source::moon::MoonlightLinkState::Failed:
    case source::moon::MoonlightLinkState::Idle:
    default:
        summary.live = models::LinkState::Saved;
        break;
    }
    return summary;
}

void AppModel::start() {
    bridge_->start();
    wifi_->autoReconnectAll();
    autoReconnectTimer_->start();
    // An immediate scan so a pad plugged in before launch is claimed promptly.
    pollUsbDirect();
    usbScanTimer_->start();
    // The first tick only baselines each tracker, so numbers appear from the
    // second tick on.
    inputRateTimer_->start();
    // Both touch D-Bus, so they belong here and not in the constructor: a test
    // that builds an AppModel must not register a tray item on the panel.
    sleepMonitor_->start();
    trayController_.start();
    // Last: the janitor pass and the staged-update scan are disk IO, and the
    // first check is 15 s out, so nothing here delays the first frame.
    updateChecker_.start();
    // A one-shot Moonlight scan beside the satellite discovery, so any
    // GameStream host on the LAN shows up in the connections list.
    moonlight_->startDiscovery();
}

void AppModel::clearPairingTarget() {
    if (!state_.pairingTarget.has_value()) { return; }
    state_.pairingTarget.reset();
    emit stateChanged();
}

void AppModel::onHubChanged() {
    state_.connections = hub_->connections();
    rebuild();
}

void AppModel::onBridgeDevicesChanged() {
    // Configure at device-add, never per event. The bridge already installed
    // its default at attach; a saved override overwrites it here. Departed
    // devices are pruned so a re-plug re-applies.
    const auto devices = bridge_->devices();
    QSet<QString> present;
    for (const auto& d : devices) {
        present.insert(d.id);
        if (deadzonePushedDevices_.contains(d.id)) { continue; }
        deadzonePushedDevices_.insert(d.id);
        if (auto dz = deadzoneRepo_.deadzonesFor(d.id)) {
            processor_.setDeadzones(d.id.toStdString(), {dz->stickFlat, dz->triggerFlat});
        }
    }
    for (auto it = deadzonePushedDevices_.begin(); it != deadzonePushedDevices_.end();) {
        it = present.contains(*it) ? std::next(it) : deadzonePushedDevices_.erase(it);
    }

    // A freshly-attached generic pad must decode under its saved remap from the
    // FIRST report, hence the re-push on every device change.
    pushJoystickRemapsToBridge();

    // An SDL device appearing or disappearing is a framework up/down signal for
    // the USB FSM, since SDL is the "framework" path here.
    syncFrameworkPresence();

    rebuild();
}

void AppModel::applyDeadzones(const QString& deviceId, const input::deadzone::Deadzones& dz) {
    processor_.setDeadzones(deviceId.toStdString(), {dz.stickFlat, dz.triggerFlat});
}

void AppModel::pushJoystickRemapsToBridge() {
    // Keys are "%04x:%04x" vid:pid strings; the bridge keys by two ints. A
    // malformed key is skipped rather than fatal, for forward-compat with a
    // future key format.
    for (const auto& [key, remap] : joystickRemapStore_.state().value()) {
        const auto colon = key.find(':');
        if (colon == std::string::npos) { continue; }
        bool okV = false;
        bool okP = false;
        const int vendorId =
            static_cast<int>(QString::fromStdString(key.substr(0, colon)).toUInt(&okV, 16));
        const int productId =
            static_cast<int>(QString::fromStdString(key.substr(colon + 1)).toUInt(&okP, 16));
        if (!okV || !okP) { continue; }
        bridge_->setJoystickRemap(vendorId, productId, remap);
    }
}

input::JoystickRemap AppModel::remapFor(int vendorId, int productId) const {
    if (const auto r = joystickRemapStore_.remapFor(vendorId, productId)) { return *r; }
    return input::JoystickRemap{};
}

void AppModel::setJoystickRemap(int vendorId, int productId, const input::JoystickRemap& remap) {
    // The store subscription re-pushes the whole set into the bridge, so the new
    // layout takes effect on the next report.
    joystickRemapStore_.setRemap(vendorId, productId, remap);
}

void AppModel::clearJoystickRemap(int vendorId, int productId) {
    // The store-driven re-push only INSTALLS present entries, never erases a
    // dropped one, so the bridge needs the explicit clear or it keeps the stale
    // remap.
    joystickRemapStore_.clearRemap(vendorId, productId);
    bridge_->clearJoystickRemap(vendorId, productId);
}

void AppModel::setInputCaptureEnabled(bool enabled) { bridge_->setJoystickCaptureEnabled(enabled); }

void AppModel::onWifiEvent(const net::ConnectionEvent& evt) {
    switch (evt.kind) {
    case net::ConnectionEventKind::PairingRequired:
        state_.pairingTarget = evt.server;
        emit stateChanged();
        break;
    case net::ConnectionEventKind::Error:
        emit errorMessage(evt.message);
        break;
    }
}

void AppModel::onUsbNotice(const reducer::UsbController& c, reducer::UsbNotice notice) {
    // Fall back to a generic noun when the FSM controller has no name yet.
    const QString name = c.name.empty() ? tr("Controller") : QString::fromStdString(c.name);
    QString msg;
    switch (notice) {
    case reducer::UsbNotice::SwitchToDirectFailed:
        msg = tr("Couldn’t switch %1 to Direct mode — keeping it on Standard.").arg(name);
        break;
    case reducer::UsbNotice::NeedsReplug:
        msg = tr("%1 needs to be unplugged and reconnected.").arg(name);
        break;
    case reducer::UsbNotice::RolledBackToDirect:
        msg = tr("%1 stayed on Direct mode.").arg(name);
        break;
    case reducer::UsbNotice::RestoreFailed:
        msg = tr("Couldn’t return %1 to Standard mode.").arg(name);
        break;
    }
    if (!msg.isEmpty()) { emit errorMessage(msg); }
}

void AppModel::pollUsbDirect() {
    if (usbManager_ == nullptr) { return; }
    // A pad that fails to claim falls back to SDL via the FSM, so an idempotent
    // re-enumeration never regresses a working SDL pad.
    usbManager_->reconcile();

    // ONE snapshot for the whole pass: controllers() returns BY VALUE, so
    // calling it per lookup would compare find()/end() iterators from two
    // different temporaries. That is UB, and the debug CRT asserts
    // "map/set iterators incompatible" the moment a Direct pad exists.
    const auto controllers = usbManager_->controllers();
    std::vector<int> present;
    for (const auto& [key, c] : controllers) {
        if (c.phase == reducer::UsbPhase::Direct && c.syntheticId.has_value()) {
            present.push_back(*c.syntheticId);
        }
    }
    const auto nowMs =
        static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
    source::usb::HidrawGateway* gw = usbGateway_.get();
    const auto updates = usbPollSampler_.sampleAll(nowMs, present, [gw](int id) -> std::int64_t {
        return gw != nullptr ? gw->completionCount(id) : 0;
    });
    // The sampler is keyed by syntheticId but the UI's slot id is the
    // controllers() map key, so translate here while both are in hand. A first
    // sample emits no update and an idle one emits 0; 0 reads as "pending".
    if (!updates.empty()) {
        QHash<int, int> bySyntheticId;
        for (const auto& u : updates) { bySyntheticId.insert(u.deviceId, u.rateHz); }
        bool changed = false;
        for (const auto& [key, c] : controllers) {
            if (c.phase != reducer::UsbPhase::Direct || !c.syntheticId.has_value()) { continue; }
            const auto it = bySyntheticId.constFind(*c.syntheticId);
            if (it == bySyntheticId.constEnd()) { continue; }
            if (usbPollRateHz_.value(key, -1) != it.value()) {
                usbPollRateHz_.insert(key, it.value());
                changed = true;
            }
        }
        // Prune departed synthetics so a stale rate can't linger on a reused key.
        for (auto it = usbPollRateHz_.begin(); it != usbPollRateHz_.end();) {
            const auto cit = controllers.find(it.key());
            const bool live =
                cit != controllers.end() && cit->second.phase == reducer::UsbPhase::Direct;
            if (live) {
                ++it;
            } else {
                it = usbPollRateHz_.erase(it);
                changed = true;
            }
        }
        if (changed) { rebuild(); }
    }
    // Claims move on this poll, and a claimed pad is what makes an endpoint
    // matchable at all. Cheap when nothing moved: the resolve compares before
    // it publishes.
    resolveAudioRoutes();
}

void AppModel::onUsbDirectChanged() {
    // Safe to call re-entrantly from a UsbDirectObserver effect: the manager
    // runs effects with its own lock released, and rebuild() only takes a fresh
    // snapshot of controllers().
    rebuild();
    resolveAudioRoutes();

    // A Direct->Standard pick parks the controller in AwaitingFramework, but on
    // the SDL twin never left bridge_->devices(), so
    // syncFrameworkPresence's appearance diff won't re-fire FrameworkUp. QUEUED
    // because this callback came from inside applyEvent's effect dispatch and
    // settling re-enters applyEvent, which would mutate FSM state mid-dispatch.
    // The settle is idempotent, so the queued pass terminates.
    QMetaObject::invokeMethod(
        this, [this] { settleAwaitingFrameworkControllers(); }, Qt::QueuedConnection);
}

void AppModel::settleAwaitingFrameworkControllers() {
    if (usbManager_ == nullptr) { return; }
    // From the LIVE device list, not the last-pass cache, so a genuinely gone
    // device is absent here and left to time out into RestoreStuck/NeedsReplug
    // rather than getting a spurious FrameworkUp.
    QSet<int> present;
    for (const auto& d : bridge_->devices()) {
        if (d.vendorId == 0 || d.productId == 0) { continue; }
        present.insert((d.vendorId << 16) | (d.productId & 0xFFFF));
    }
    for (const auto& [key, c] : usbManager_->controllers()) {
        if (reducer::shouldSettleAwaitingFramework(c.phase, present.contains(key))) {
            usbManager_->onFrameworkUp(c.vendorId, c.productId, key);
        }
    }
}

void AppModel::syncFrameworkPresence() {
    if (usbManager_ == nullptr) { return; }
    // The FSM's bind effect needs a frameworkId, so the model's vpKey stands in
    // as a stable surrogate: this path binds by slot-id string, not the
    // numeric id, so only the up/down transitions matter.
    QSet<int> current;
    for (const auto& d : bridge_->devices()) {
        if (d.vendorId == 0 || d.productId == 0) { continue; }
        const int key = (d.vendorId << 16) | (d.productId & 0xFFFF);
        current.insert(key);
        if (!lastFrameworkVpKeys_.contains(key)) {
            usbManager_->onFrameworkUp(d.vendorId, d.productId, key);
        }
    }
    for (int key : lastFrameworkVpKeys_) {
        if (!current.contains(key)) {
            const int vendorId = (key >> 16) & 0xFFFF;
            const int productId = key & 0xFFFF;
            usbManager_->onFrameworkDown(vendorId, productId);
        }
    }
    lastFrameworkVpKeys_ = current;

    // Level-triggered, not only the appearance edge above: the SDL twin is
    // continuously present while a pad is claimed for Direct, so a controller
    // parked in AwaitingFramework by a release would never see a fresh edge. A
    // genuinely gone device is not in `current`, so its Timeout path is intact.
    for (const auto& [key, c] : usbManager_->controllers()) {
        if (reducer::shouldSettleAwaitingFramework(c.phase, current.contains(key))) {
            usbManager_->onFrameworkUp(c.vendorId, c.productId, key);
        }
    }
}

void AppModel::rebuild() {
    QList<models::ControllerSlot> next;
    // Every slot this pass SHOWS, with the USB identity of the pad behind it.
    // Published before the binding cross-reference, because both the
    // emulation-type seed and the binding-presence gate ask it which pad is
    // actually behind a slot id. The desktop clients seed no virtual slot.
    std::vector<reducer::PresentSlot> presentPads;

    // Twin-dedup: a pad visible to BOTH SDL and the raw-HID gateway must
    // stream via exactly one path. The hidden set goes into the bridge so those
    // ids never reach the wire, their slots are dropped so they get no binding,
    // and the synthetics are added in their place.
    std::vector<reducer::SyntheticTwin> synthetics;
    std::map<int, reducer::UsbController> controllers;
    if (usbManager_ != nullptr) { controllers = usbManager_->controllers(); }
    for (const auto& [key, c] : controllers) {
        if (c.phase == reducer::UsbPhase::Direct) {
            synthetics.push_back({c.vendorId, c.productId});
        }
    }
    const auto sdlDevices = bridge_->devices();
    std::vector<reducer::RoutedDevice> routed;
    routed.reserve(static_cast<std::size_t>(sdlDevices.size()));
    for (const auto& d : sdlDevices) {
        routed.push_back(
            {d.id.toStdString(), d.vendorId, d.productId, /*disconnecting=*/false, d.bluetooth});
    }
    const std::set<std::string> hidden = reducer::suppressedRoutedIds(synthetics, routed);
    {
        std::unordered_set<std::string> hiddenSet(hidden.begin(), hidden.end());
        bridge_->setSuppressedDeviceIds(hiddenSet);
    }

    for (const auto& d : sdlDevices) {
        // Skip the SDL twin of an active USB-direct claim — it streams via raw-HID.
        if (hidden.count(d.id.toStdString()) != 0) { continue; }
        models::ControllerSlot s;
        s.id = d.id;
        s.name = d.name;
        s.capabilities.hasMotion = d.motionCapable;
        s.capabilities.hasLightbar = d.hasLightbar;
        s.capabilities.hasTouchpad = d.hasTouchpad;
        s.capabilities.hasRumble = d.hasRumble;
        // Only meaningful on the Direct path, and a Bluetooth pad has none.
        s.verifiedModel = !d.bluetooth && usbGateway_ != nullptr &&
                          usbGateway_->isKnownFastLaneModel(d.vendorId, d.productId);
        s.capabilities.batteryLevel = d.batteryLevel;
        s.capabilities.batteryStatus = d.batteryStatus;
        // Only a raw joystick decodes through the remappable mapJoystick path;
        // synthetics below keep the false default.
        s.remappable = d.isRawJoystick;
        s.bluetooth = d.bluetooth;
        stampSlotPath(s, d.vendorId, d.productId, d.bluetooth, controllers);
        presentPads.push_back({d.id.toStdString(), d.vendorId, d.productId});
        next.append(s);
    }

    // One slot per USB-direct-claimed pad. Its id is the model key string the
    // read loop publishes under, so binding it routes the decoded reports
    // through the existing hub machinery.
    for (const auto& [key, c] : controllers) {
        if (c.phase != reducer::UsbPhase::Direct) { continue; }
        models::ControllerSlot s;
        s.id = QString::fromStdString(std::to_string(key));
        s.name = QString::fromStdString(c.name);
        const auto parser = input::usbparse::parserForDevice(c.vendorId, c.productId);
        s.capabilities.hasMotion = input::usbparse::parserHasImu(parser);
        s.capabilities.hasLightbar = false;
        // The raw-HID decoder reads the DS4 / DualSense touch block directly, so
        // a claimed pad of that family keeps the touchpad its SDL twin had.
        s.capabilities.hasTouchpad = input::usbparse::parserHasTouchpad(parser);
        // The pad's motors, not the path's actuator: the wire fold drops rumble
        // for a Direct claim, but the capability table's input layer still says
        // the hardware exists so the refusal lands on the Link row.
        s.capabilities.hasRumble = input::usbparse::parserHasRumble(parser);
        s.verifiedModel =
            usbGateway_ != nullptr && usbGateway_->isKnownFastLaneModel(c.vendorId, c.productId);
        s.usbDirect = true;
        s.liveRates.directPollHz = usbPollRateHz_.value(key, 0);
        // A synthetic IS a USB-direct controller, so it is always
        // path-supported; the mapper reads phase/desired/failure off its own
        // entry, keyed by vid/pid, which round-trips to this key.
        stampSlotPath(s, c.vendorId, c.productId, /*bluetooth=*/false, controllers);
        presentPads.push_back({s.id.toStdString(), c.vendorId, c.productId});
        next.append(s);
    }

    // A tracked model whose stand-alone identity is not a gamepad (the Steam
    // Controller emulates a keyboard and mouse) never gets an SDL row and has
    // no synthetic until a claim succeeds — without a card of its own there
    // would be nothing to pick Direct from. Same id as its future synthetic, so
    // the binding and type choice survive the claim.
    for (const auto& [key, c] : controllers) {
        if (c.frameworkExpected || c.phase == reducer::UsbPhase::Direct) { continue; }
        models::ControllerSlot s;
        s.id = QString::fromStdString(std::to_string(key));
        s.name = QString::fromStdString(c.name);
        const auto parser = input::usbparse::parserForDevice(c.vendorId, c.productId);
        s.capabilities.hasMotion = input::usbparse::parserHasImu(parser);
        s.capabilities.hasTouchpad = input::usbparse::parserHasTouchpad(parser);
        s.capabilities.hasRumble = input::usbparse::parserHasRumble(parser);
        s.capabilities.hasLightbar = false;
        stampSlotPath(s, c.vendorId, c.productId, /*bluetooth=*/false, controllers);
        presentPads.push_back({s.id.toStdString(), c.vendorId, c.productId});
        next.append(s);
    }

    // BEFORE the cross-reference below: resolveControllerType reads this to seed
    // the type off the pad's own USB identity.
    presentPads_ = std::move(presentPads);

    const auto bindings = hub_->bindings();
    for (auto& s : next) {
        // The mute control shows exactly where the descriptor claims a mic:
        // the same fold the hub's capability fn makes (route AND toggle), so
        // the card and the wire cannot disagree about whether a microphone
        // exists to mute. The muted flag is the LOCAL truth, host lamp writes
        // notwithstanding.
        s.micArmed = slotCarriesMicSource(s.id) && micEnabledStore_.isEnabled(s.id.toStdString());
        s.micMuted = micMuteStore_.isMuted(s.id.toStdString());
        const auto cid = bindings.value(s.id);
        // A Moonlight binding is a binding: the card, the accounting and the
        // apply readback all key on boundConnectionId, so a pad driving a
        // GameStream host has to report one too. The two tables are exclusive.
        const QString moonHost = cid.isEmpty() ? moonlight_->boundHostFor(s.id) : QString();
        if (!moonHost.isEmpty()) {
            s.boundConnectionId = moonHost;
            s.boundStatus = moonlightSummary(moonHost);
        } else if (!cid.isEmpty()) {
            s.boundConnectionId = cid;
            s.boundStatus = hub_->summary(cid);
            // Server-localized catalog text; left empty, and the suffix
            // omitted, when no catalog row matches.
            const int type = resolveControllerType(s.id);
            for (const auto& t : pickableTypesFor(s.id)) {
                if (t.type == type) {
                    s.emulateName = t.shortName;
                    break;
                }
            }
        }
        // Preserve the measured stream rates across the rebuild. directPollHz
        // was already set on the synthetic above, so keep it.
        const auto rit = liveRatesBySlot_.constFind(s.id);
        if (rit != liveRatesBySlot_.constEnd()) {
            const int keepPoll = s.liveRates.directPollHz;
            s.liveRates = rit.value();
            s.liveRates.directPollHz = keepPoll;
        }
    }
    state_.slotList = std::move(next);

    syncInputRateDevices();

    // Topology rides REST with no per-add UDP ACK poll, so "busy" is a session
    // in its Linking handshake.
    bool busy = false;
    for (auto* conn : wifi_->connections()) {
        if (conn->state() == net::SessionState::Linking) {
            busy = true;
            break;
        }
    }
    state_.busy = busy;

    QHash<QString, net::ConnectionHub::ReportSender> nextRouting;
    QHash<QString, net::ConnectionHub::MotionSender> nextMotion;
    QHash<QString, net::ConnectionHub::BatterySender> nextBattery;
    QHash<QString, net::ConnectionHub::TouchpadSender> nextTouchpad;
    for (const auto& slot : state_.slotList) {
        if (auto sender = hub_->reportSenderForSlot(slot.id)) {
            nextRouting.insert(slot.id, sender);
        }
        if (auto sender = hub_->motionSenderForSlot(slot.id)) {
            nextMotion.insert(slot.id, sender);
        }
        if (auto sender = hub_->batterySenderForSlot(slot.id)) {
            nextBattery.insert(slot.id, sender);
        }
        if (auto sender = hub_->touchpadSenderForSlot(slot.id)) {
            nextTouchpad.insert(slot.id, sender);
        }
    }
    {
        std::lock_guard<std::mutex> lock(routingMtx_);
        routing_ = std::move(nextRouting);
        motionRouting_ = std::move(nextMotion);
        batteryRouting_ = std::move(nextBattery);
        touchpadRouting_ = std::move(nextTouchpad);
    }

    // The composer's distinct-until-changed plus the inhibitor's idempotent
    // acquire/release keep a noisy hub feed that doesn't move the count from
    // ever touching the OS power portal.
    QHash<QString, models::LinkState> connectionStates;
    for (const auto& summary : state_.connections) {
        connectionStates.insert(summary.id, summary.live);
    }
    const int nextStreaming = composer::streamingSlotCount(bindings, connectionStates);
    // A session opening is activity: the pad the user is about to pick up has
    // not reported yet, so the idle window must not already be running down.
    if (nextStreaming > streamingSlotCount_.value()) {
        controllerActivity_.noteActivityAt(steadyNowMs());
    }
    streamingSlotCount_.set(nextStreaming);

    // Mute is a live control over a present pad: a departed slot's entry is
    // dropped so a replugged pad (which reuses its model-keyed id) comes back
    // live, the way the hardware itself does.
    {
        std::set<std::string> presentIds;
        for (const auto& s : state_.slotList) { presentIds.insert(s.id.toStdString()); }
        micMuteStore_.retainOnly(presentIds);
    }

    // Every input the audio eligibility rules read funnels through this
    // function (bindings, session states, toggles via re-bind, the probe
    // verdict via poolChanged, mute via setSlotMicMuted), so the engines
    // converge here and nowhere else.
    reconcileAudioEngines();

    emit stateChanged();

    // Rides the same rebuild the Live transition triggered; the reducer's edge
    // guard makes the steady state free.
    prewarmCatalogs();

    // Last, because it can bind/unbind and therefore re-enter this function.
    applyBindingPresence();
}

std::optional<std::pair<int, int>> AppModel::boundPadIdentity(const QString& slotId) const {
    const std::string id = slotId.toStdString();
    // A slot on screen answers for itself.
    if (const auto shown = reducer::padIdentityFor(id, presentPads_)) { return shown; }
    // A framework twin hidden by an active claim is absent from the slot list
    // but still enumerated by SDL: a pad that MOVED, not one that left.
    for (const auto& d : bridge_->devices()) {
        if (d.id != slotId) { continue; }
        const std::optional<std::pair<int, int>> identity = std::make_pair(d.vendorId, d.productId);
        return reducer::isPadIdentity(identity) ? identity : std::nullopt;
    }
    // A synthetic slot id packs its own (vid, pid), so a just-released claim
    // still names the pad it belonged to. Whether that pad is still here is the
    // gate's question, not this one's.
    const std::optional<std::pair<int, int>> packed = reducer::parseSyntheticSlotId(id);
    if (reducer::isPadIdentity(packed)) { return packed; }
    return std::nullopt;
}

AppModel::SlotHardware AppModel::slotHardware(const QString& slotId) const {
    SlotHardware hw;
    // A synthetic id packs its own (vid, pid); the parser family is the
    // hardware truth for what the claim decodes. Checked first because it needs
    // no lock and a synthetic id can never collide with an "sdl:N" id.
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (vp.has_value() && reducer::isPadIdentity(vp)) {
        const auto parser = input::usbparse::parserForDevice(vp->first, vp->second);
        hw.usbDirect = true;
        hw.hasMotion = input::usbparse::parserHasImu(parser);
        hw.hasTouchpad = input::usbparse::parserHasTouchpad(parser);
        hw.hasRumble = input::usbparse::parserHasRumble(parser);
        // The Direct path now has an OUT report path, so a claimed pad answers
        // for its real actuators instead of a flat false. Whether it can drive
        // them right now is the router's call (the claim has to be live).
        hw.hasLightbar = input::usbout::parserHasLightbar(parser);
        hw.hasTriggerEffects = input::usbout::parserHasTriggerEffects(parser);
        hw.hasPlayerLeds = input::usbout::parserHasPlayerLeds(parser);
        return hw;
    }
    for (const auto& d : bridge_->devices()) {
        if (d.id != slotId) { continue; }
        hw.hasMotion = d.motionCapable;
        hw.hasLightbar = d.hasLightbar;
        hw.hasTouchpad = d.hasTouchpad;
        hw.hasRumble = d.hasRumble;
        // Left false: SDL has no adaptive-trigger or player-LED call, so a pad
        // on the Standard path cannot land either however good its hardware is.
        return hw;
    }
    return hw;
}

std::uint8_t AppModel::slotBatteryLevel(const QString& slotId) const {
    // The bridge is the only publisher: a synthetic slot's charge arrives on the
    // same battery stream, keyed by the same slot id.
    for (const auto& d : bridge_->devices()) {
        if (d.id == slotId) { return d.batteryLevel; }
    }
    return net::SatelliteClient::kBatteryLevelUnknown;
}

reducer::SlotFeedbackInputs AppModel::feedbackInputs(const QString& slotId) const {
    const SlotHardware hw = slotHardware(slotId);
    reducer::SlotFeedbackInputs in;
    in.usbDirect = hw.usbDirect;
    in.padRumble = hw.hasRumble;
    in.padLightbar = hw.hasLightbar;
    in.padTriggerEffects = hw.hasTriggerEffects;
    in.padPlayerLeds = hw.hasPlayerLeds;
    if (hw.usbDirect) {
        const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
        in.directClaimLive = vp.has_value() && usbManager_ != nullptr &&
                             usbManager_->isDirectClaimed(vp->first, vp->second);
        if (vp.has_value()) {
            const auto parser = input::usbparse::parserForDevice(vp->first, vp->second);
            in.padMicLed = input::usbout::parserHasMicMuteLed(parser);
        }
        // The matcher's live answer: whether THIS pad's own endpoints were
        // confidently named on this machine. The single fold both the
        // descriptor caps and the engines read.
        const auto route = audioRouteForSlot(slotId);
        in.padMicRoute = route.microphone;
        in.padSpeakerRoute = route.speaker;
    }
    return in;
}

bool AppModel::slotCarriesMicSource(const QString& slotId) const {
    return reducer::slotCarriesMicCapture(feedbackInputs(slotId));
}

bool AppModel::slotCarriesSpeakerSink(const QString& slotId) const {
    return reducer::slotCarriesSpeakerPlayout(feedbackInputs(slotId));
}

reducer::HostAudioVerdict AppModel::hostControllerAudioFor(const QString& hostId) const {
    const auto* conn = wifi_->get(hostId);
    if (conn == nullptr) { return {}; } // conservative: no probe, no audio
    return {conn->hostMicAvailable(), conn->hostSpeakerAvailable()};
}

void AppModel::actuateRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                             std::uint16_t durationMs) {
    const auto in = feedbackInputs(slotId);
    switch (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::Rumble)) {
    case reducer::FeedbackTarget::Standard:
        // applyRumble marshals the actuation onto the SDL thread internally.
        bridge_->applyRumble(slotId, strong, weak, durationMs);
        return;
    case reducer::FeedbackTarget::DirectUsb: {
        // No duration on the raw path: the pad's motors run until the next
        // write, and the satellite refreshes well before its own expiry. A
        // stop is magnitudes 0,0, which arrives as an ordinary report.
        const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
        if (vp.has_value() && usbManager_ != nullptr) {
            usbManager_->applyRumble(vp->first, vp->second, strong, weak);
        }
        return;
    }
    case reducer::FeedbackTarget::None:
        return;
    }
}

void AppModel::actuateLightbar(const QString& slotId, std::uint8_t r, std::uint8_t g,
                               std::uint8_t b) {
    const auto in = feedbackInputs(slotId);
    switch (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::Lightbar)) {
    case reducer::FeedbackTarget::Standard:
        bridge_->applyLightbar(slotId, r, g, b);
        return;
    case reducer::FeedbackTarget::DirectUsb: {
        const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
        if (vp.has_value() && usbManager_ != nullptr) {
            usbManager_->applyLightbar(vp->first, vp->second, r, g, b);
        }
        return;
    }
    case reducer::FeedbackTarget::None:
        return;
    }
}

void AppModel::actuateTriggerEffects(
    const QString& slotId, const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& left,
    const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& right) {
    const auto in = feedbackInputs(slotId);
    if (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::TriggerEffects) !=
        reducer::FeedbackTarget::DirectUsb) {
        return;
    }
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (vp.has_value() && usbManager_ != nullptr) {
        usbManager_->applyTriggerEffects(vp->first, vp->second, left.data(), right.data());
    }
}

void AppModel::actuatePlayerLeds(const QString& slotId, std::uint8_t ledMask) {
    const auto in = feedbackInputs(slotId);
    if (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::PlayerLeds) !=
        reducer::FeedbackTarget::DirectUsb) {
        return;
    }
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (vp.has_value() && usbManager_ != nullptr) {
        usbManager_->applyPlayerLeds(vp->first, vp->second, ledMask);
    }
}

void AppModel::actuateMicLed(const QString& slotId, std::uint8_t state) {
    const auto in = feedbackInputs(slotId);
    if (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::MicLed) !=
        reducer::FeedbackTarget::DirectUsb) {
        return;
    }
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (vp.has_value() && usbManager_ != nullptr) {
        usbManager_->applyMicMuteLed(vp->first, vp->second, state);
    }
}

audio::PadAudioRoute AppModel::audioRouteForSlot(const QString& slotId) const {
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (!vp.has_value()) { return {}; }
    std::lock_guard<std::mutex> lock(audioRoutesMtx_);
    const auto it = padAudioRoutes_.find(audio::padAudioKey(vp->first, vp->second));
    if (it == padAudioRoutes_.end()) { return {}; }
    return it->second;
}

void AppModel::resolveAudioRoutes() {
    if (usbManager_ == nullptr) { return; }
    std::vector<audio::AudioPadCandidate> pads;
    for (const auto& [key, c] : usbManager_->controllers()) {
        // Claimed pads only: the physical-pad audio path is the Direct claim's
        // (the SDL twin never carries the iProduct string to match on).
        if (c.phase != reducer::UsbPhase::Direct || !c.syntheticId.has_value()) { continue; }
        const auto parser = input::usbparse::parserForDevice(c.vendorId, c.productId);
        pads.push_back(audio::AudioPadCandidate{c.vendorId, c.productId, c.name,
                                                input::usbparse::parserHasUsbAudio(parser)});
    }
    auto routes = audio::resolvePadAudioRoutes(pads, audioGateway_.captureDeviceNames(),
                                               audioGateway_.playbackDeviceNames());
    {
        std::lock_guard<std::mutex> lock(audioRoutesMtx_);
        if (routes == padAudioRoutes_) { return; }
        padAudioRoutes_ = std::move(routes);
    }
    // The caps fold reads the routes, so every bound synthetic slot's
    // descriptor has to re-fold and re-PUT — this is the "an endpoint
    // appeared/vanished re-declares the slot" edge, and re-binding is the
    // machinery that already exists for it (attachSlot detects the change and
    // converges via the per-controller PUT).
    const auto bindings = hub_->bindings();
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        if (reducer::parseSyntheticSlotId(it.key().toStdString()).has_value()) {
            republishSlotCaps(it.key());
        }
    }
    rebuild();
}

void AppModel::republishSlotCaps(const QString& slotId) {
    const auto bound = hub_->boundConnection(slotId);
    if (!bound.has_value()) { return; }
    hub_->bind(slotId, bound->id);
}

void AppModel::setSlotMicMuted(const QString& slotId, bool muted) {
    if (slotId.isEmpty()) { return; }
    micMuteStore_.setMuted(slotId.toStdString(), muted);
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (vp.has_value() && usbManager_ != nullptr) {
        // Keep the wire latch in step so kXusbMicMute holds the same state the
        // app shows; a no-op when the pad's own button was the writer.
        usbManager_->setPadMicMuted(vp->first, vp->second, muted);
    }
    // The lamp answers immediately and locally — a mute that waits on a host
    // round-trip feels dead. A host MSG_MIC_LED landing later repaints it
    // (last writer wins on the PAD; the UI keeps showing the local truth).
    actuateMicLed(slotId, muted ? proto::kMicLedStateOn : proto::kMicLedStateOff);
    // Restamps the slot card and re-runs the engine reconcile: muted closes
    // the capture device, unmuted reopens it.
    rebuild();
}

void AppModel::toggleSlotMicMute(const QString& slotId) {
    if (slotId.isEmpty()) { return; }
    setSlotMicMuted(slotId, !micMuteStore_.isMuted(slotId.toStdString()));
}

void AppModel::onPadMicMuteChanged(int vendorId, int productId, bool muted) {
    // The synthetic slot id IS the model key's string form.
    const QString slotId =
        QString::fromStdString(std::to_string((vendorId << 16) | (productId & 0xFFFF)));
    // The latch already moved (the decoder owns it); this mirrors the store,
    // drives the lamp and reconciles the engines through the same single door
    // the UI click uses.
    setSlotMicMuted(slotId, muted);
}

void AppModel::reconcileAudioEngines() {
    std::vector<source::audio::MicCaptureTarget> micTargets;
    std::vector<source::audio::SpeakerVoiceTarget> speakerVoices;

    for (const auto& s : state_.slotList) {
        if (!s.usbDirect || !s.boundConnectionId.has_value()) { continue; }
        auto* conn = wifi_->get(*s.boundConnectionId);
        if (conn == nullptr) { continue; }
        const auto route = audioRouteForSlot(s.id);
        const std::string slotId = s.id.toStdString();
        // Faltering counts as streaming: it is a session riding out missed
        // acks, and tearing audio down two seconds before the input stream
        // would flap on every blip.
        const bool streaming = conn->state() == net::SessionState::Live ||
                               conn->state() == net::SessionState::Faltering;

        audio::AudioSlotFacts mic;
        mic.streaming = streaming;
        mic.toggleOn = micEnabledStore_.isEnabled(slotId);
        mic.routeMatched = route.microphone;
        mic.hostCarries = conn->hostMicAvailable();
        mic.muted = micMuteStore_.isMuted(slotId);
        if (audio::micCaptureEligible(mic)) {
            source::audio::MicCaptureTarget target;
            target.slotId = slotId;
            target.captureDeviceName = route.captureDeviceName;
            if (auto sender = hub_->micAudioSenderForSlot(s.id)) {
                target.send = std::move(sender);
                micTargets.push_back(std::move(target));
            }
        }

        audio::AudioSlotFacts speaker;
        speaker.streaming = streaming;
        speaker.toggleOn = speakerEnabledStore_.isEnabled(slotId);
        speaker.routeMatched = route.speaker;
        speaker.hostCarries = conn->hostSpeakerAvailable();
        if (audio::speakerPlayoutEligible(speaker)) {
            const auto descriptor = conn->descriptorFor(s.id);
            if (descriptor.has_value()) {
                source::audio::SpeakerVoiceTarget voice;
                voice.connectionId = s.boundConnectionId->toStdString();
                voice.controllerIndex = descriptor->ctrlIdx;
                voice.slotId = slotId;
                voice.playbackDeviceName = route.playbackDeviceName;
                speakerVoices.push_back(std::move(voice));
            }
        }
    }

    micEngine_.reconcile(micTargets);
    speakerEngine_.reconcile(speakerVoices);
}

void AppModel::applyBindingPresence() {
    if (bindingPresenceInFlight_) { return; }
    const auto bindings = hub_->bindings();
    std::vector<reducer::BoundSlot> rows;
    rows.reserve(static_cast<std::size_t>(bindings.size()));
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        reducer::BoundSlot row;
        row.slotId = it.key().toStdString();
        row.connId = it.value().toStdString();
        row.identity = boundPadIdentity(it.key());
        rows.push_back(std::move(row));
    }
    const auto actions = reducer::resolveBindingPresence(presentPads_, rows);
    if (actions.empty()) { return; }

    bindingPresenceInFlight_ = true;
    // Composed BEFORE the detach: unbind() re-derives the hub's summaries, so
    // the satellite's label must be read while the binding still exists. A
    // migration is silent on purpose — the pad never left, only its slot id
    // changed, and announcing that trains the user to ignore the channel.
    QStringList notices;
    for (const auto& action : actions) {
        if (action.kind == reducer::BindingPresenceKind::Unbind) {
            const auto summary = hub_->summary(QString::fromStdString(action.connId));
            notices.append(summary ? summary->label : QString());
        }
        // unbind() DELETEs the controller on a live session, so the phantom
        // leaves the satellite now rather than at the next reaper timeout.
        hub_->unbind(QString::fromStdString(action.slotId));
        if (action.kind == reducer::BindingPresenceKind::Migrate) {
            hub_->bind(QString::fromStdString(action.toSlotId),
                       QString::fromStdString(action.connId));
        }
    }
    bindingPresenceInFlight_ = false;

    // Only now, with the guard clear and the state settled: a toast is UI, and
    // whatever it reaches must be free to call back into this model.
    for (const auto& label : notices) {
        emit errorMessage(label.isEmpty()
                              ? tr("Controller disconnected — its binding was removed.")
                              : tr("Controller disconnected — unbound from %1.").arg(label));
    }
}

void AppModel::syncInputRateDevices() {
    if (!inputRateStore_) { return; }
    QSet<QString> present;
    for (const auto& s : state_.slotList) {
        present.insert(s.id);
        inputRateStore_->addDevice(s.id.toStdString()); // idempotent
    }
    // Drop departed slots so a later re-attach re-baselines from 0 instead of
    // inheriting a stale anchor.
    for (auto it = liveRatesBySlot_.begin(); it != liveRatesBySlot_.end();) {
        if (present.contains(it.key())) {
            ++it;
        } else {
            inputRateStore_->removeDevice(it.key().toStdString());
            it = liveRatesBySlot_.erase(it);
        }
    }
}

void AppModel::onInputRatesChanged(const source::SlotInputRatesMap& rates) {
    // Cached so a later slot-list rebuild keeps the numbers. directPollHz is
    // owned by the USB poll path, not the store, so it is preserved here.
    bool changed = false;
    for (const auto& [slotId, r] : rates) {
        const QString id = QString::fromStdString(slotId);
        models::SlotLiveRates next = liveRatesBySlot_.value(id);
        next.gamepadHz = r.gamepadHz;
        next.gamepadPeakHz = r.gamepadPeakHz;
        next.motionHz = r.motionHz;
        next.motionPeakHz = r.motionPeakHz;
        if (liveRatesBySlot_.value(id) != next) {
            liveRatesBySlot_.insert(id, next);
            changed = true;
        }
    }
    if (!changed) { return; }
    // Patched in place rather than through rebuild(): a pure rate change needs
    // no routing or twin-dedup recompute.
    bool visibleChange = false;
    for (auto& s : state_.slotList) {
        const auto rit = liveRatesBySlot_.constFind(s.id);
        if (rit == liveRatesBySlot_.constEnd()) { continue; }
        models::SlotLiveRates merged = rit.value();
        merged.directPollHz = s.liveRates.directPollHz;
        if (s.liveRates != merged) {
            s.liveRates = merged;
            visibleChange = true;
        }
    }
    if (visibleChange) { emit stateChanged(); }
}

int AppModel::resolveControllerType(const QString& slotId) const {
    const QString connId = hub_->bindings().value(slotId);
    if (connId.isEmpty()) { return proto::kControllerTypeXbox; }
    return currentTypeForConnection(connId, slotId);
}

int AppModel::currentTypeFor(const QString& slotId) const { return resolveControllerType(slotId); }

int AppModel::currentTypeForConnection(const QString& connId, const QString& slotId) const {
    if (connId.isEmpty()) { return proto::kControllerTypeXbox; }
    const auto userOverride = typeStore_.typeFor(connId.toStdString(), slotId.toStdString());
    std::optional<models::CatalogDto> cached;
    if (auto* conn = wifi_->get(connId)) { cached = catalogRepo_.cached(conn->server().id()); }
    // The PAD'S OWN USB identity has to reach the seed, or the ladder skips the
    // catalog's `emulates` hints and every pad falls through to the first
    // offered type — which is how a Switch Pro ends up declared as a virtual
    // DualShock 4. SDL exposes no type slug through the bridge, so only the usb
    // half of the hint is matched; an unresolvable pad passes an empty key.
    const auto identity = boundPadIdentity(slotId);
    const QString vidPid =
        identity ? reducer::vidPidKey(identity->first, identity->second) : QString();
    return reducer::seedControllerType(userOverride, cached, /*sdlTypeSlug=*/QString(), vidPid);
}

QList<composer::PickableType> AppModel::pickableTypesFor(const QString& slotId) const {
    return pickableTypesForConnection(hub_->bindings().value(slotId));
}

QList<composer::PickableType> AppModel::pickableTypesForConnection(const QString& connId) const {
    if (connId.isEmpty()) { return {}; }
    if (auto* conn = wifi_->get(connId)) {
        if (auto cached = catalogRepo_.cached(conn->server().id())) {
            return composer::offerableTypes(*cached);
        }
    }
    // Fall back to the last fetched catalog, so a freshly-bound slot still
    // offers the known types.
    return catalogComposer_.state().value();
}

void AppModel::setSlotControllerType(const QString& slotId, int type) {
    const QString connId = hub_->bindings().value(slotId);
    if (connId.isEmpty()) { return; } // unbound: nothing to emulate
    typeStore_.setType(connId.toStdString(), slotId.toStdString(), type);
    // Re-attach so the new descriptor is PUT: bind() re-reads
    // resolveControllerType, which now returns the override.
    hub_->bind(slotId, connId);
}

// The repository is keyed on the stable satellite id, which IS the connection
// id the binding surfaces carry, so these need no wifi_ lookup.

bool AppModel::hasCatalogFor(const QString& hostId) const {
    return hostId.isEmpty() ? false : catalogRepo_.cached(hostId).has_value();
}

std::optional<models::CatalogTypeDto> AppModel::catalogTypeFor(const QString& hostId,
                                                               int type) const {
    if (hostId.isEmpty()) { return std::nullopt; }
    const auto cached = catalogRepo_.cached(hostId);
    if (!cached.has_value()) { return std::nullopt; }
    for (const auto& t : cached->controllerTypes) {
        if (t.id == type) { return t; }
    }
    return std::nullopt;
}

QHash<QString, models::CatalogHostFeatureDto>
AppModel::catalogHostFeatures(const QString& hostId) const {
    if (hostId.isEmpty()) { return {}; }
    const auto cached = catalogRepo_.cached(hostId);
    if (!cached.has_value()) { return {}; }
    return cached->hostFeatures;
}

void AppModel::refreshCatalogForSlot(const QString& slotId) {
    refreshCatalogForConnection(hub_->bindings().value(slotId));
}

void AppModel::refreshCatalogForConnection(const QString& connId) {
    if (connId.isEmpty()) { return; }
    auto* conn = wifi_->get(connId);
    if (conn == nullptr) { return; }
    const auto server = conn->server();
    const QString satId = server.id();
    // The satellite falls back to en if it can't serve this chain.
    const QString acceptLanguage = QLocale().bcp47Name();
    // Loading keeps any prior catalog as stale, so the picker shows a spinner
    // over the last-known types rather than an empty list.
    catalogState_ = core::toLoading(catalogState_);
    emit catalogStateChanged();
    catalogRepo_.catalogFor(server, satId, acceptLanguage,
                            [this](const source::CatalogState& state) {
                                catalogState_ = state;
                                // Feed the composer on fresh, 304-revalidated
                                // AND stale-served-on-error data; its
                                // distinct-until-changed suppresses no-ops.
                                if (state.hasData()) {
                                    catalogSnapshot_.set(composer::CatalogSnapshot{*state.data});
                                }
                                emit catalogStateChanged();
                            });
}

void AppModel::prewarmCatalogs() {
    std::vector<reducer::CatalogLink> links;
    const auto conns = wifi_->connections();
    links.reserve(static_cast<std::size_t>(conns.size()));
    for (auto* conn : conns) {
        links.emplace_back(conn->server().id(), conn->state() == net::SessionState::Live);
    }
    const auto targets = reducer::catalogPrewarmTargets(links, prewarmedCatalogs_);
    if (targets.empty()) { return; }
    const QString acceptLanguage = QLocale().bcp47Name();
    for (const auto& satId : targets) {
        auto* conn = wifi_->get(satId);
        if (conn == nullptr) { continue; }
        // Unlike refreshCatalogForConnection this never touches catalogState_:
        // a background warm must not flip the picker into Loading. The snapshot
        // still updates so an already-open picker sees fresh types.
        catalogRepo_.catalogFor(
            conn->server(), satId, acceptLanguage, [this](const source::CatalogState& state) {
                if (state.hasData()) {
                    catalogSnapshot_.set(composer::CatalogSnapshot{*state.data});
                }
            });
    }
}

} // namespace dish
