// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "FeatureSettings.h"
#include "Input/GamepadInputProcessor.h"
#include "Input/SDLGamepadBridge.h"
#include "Models/Models.h"
#include "Network/ConnectionHub.h"
#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"
#include "architecture/Observable.h"
#include "composer/CatalogComposer.h"
#include "composer/BackgroundCoordinator.h"
#include "composer/ConnectionCoordinator.h"
#include "composer/CrashReportingBackend.h"
#include "composer/CrashReportingController.h"
#include "composer/SleepCoordinator.h"

#include "composer/ThemeController.h"
#include "composer/TrayComposer.h"
#include "composer/TrayController.h"
#include "composer/WakeStateComposer.h"
#include "composer/WakeStateController.h"
#include "repository/DeadzoneRepository.h"
#include "repository/MotionPreferenceRepository.h"
#include "core/reducer/BindingPresence.h"
#include "core/reducer/PollRateSampler.h"
#include "source/inputrate/InputRateStore.h"
#include "source/http/SatelliteCatalogRepository.h"
#include "source/notification/DesktopNotifier.h"
#include "source/store/BackgroundPreferenceStore.h"
#include "source/store/ControllerTypeStore.h"
#include "source/store/CrashReportingStore.h"
#include "source/store/JoystickRemapStore.h"
#include "source/store/MotionEnabledStore.h"
#include "source/store/OnboardingPreferenceStore.h"
#include "source/store/TouchpadModeStore.h"
#include "source/store/ThemePreferenceStore.h"
#include "source/store/UpdatePreferenceStore.h"
#include "source/store/UsbPathPreferenceStore.h"
#include "source/system/SleepMonitor.h"
#include "source/tray/StatusNotifierTrayIcon.h"
#include "source/tray/TrayIcon.h"
#include "source/usb/UsbGamepadManager.h"
#include "source/usb/HidrawGateway.h"
#include "update/UpdateChecker.h"
#include "Util/DisplaySleepInhibitor.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace dish {

// The single immutable state slice the UI consumes, mirroring dish-android's
// MainUiState so every client exposes one canonical object rather than a fan of
// independent fields and signals.
struct MainUiState {
    // Named `slotList`, not `slots`: moc treats `slots` as a reserved keyword
    // via Q_SLOTS and won't parse a member with that name.
    QList<models::ControllerSlot> slotList;
    QList<models::ConnectionSummary> connections;
    std::optional<models::DiscoveredServer> pairingTarget;
    bool busy = false;
};

// Owns the network and input layers and stitches them together. Maintains a
// slotId -> WifiConnection routing table written on the Qt main thread and read
// from the SDL gamepad thread on every report.
class AppModel : public QObject {
    Q_OBJECT
  public:
    // The unique_ptr overload lets tests inject a fake inhibitor.
    explicit AppModel(QObject* parent = nullptr);
    AppModel(std::unique_ptr<util::DisplaySleepInhibitor> inhibitor, QObject* parent = nullptr);
    ~AppModel() override;

    net::ConnectionStore* store() { return store_.get(); }
    net::WifiConnectionManager* wifi() { return wifi_; }
    net::ConnectionHub* hub() { return hub_; }
    // The reactive/command surface the UI binds to. Hot-path binding and
    // routing still live on hub().
    composer::ConnectionCoordinator* connections() { return connections_; }
    input::GamepadInputProcessor* processor() { return &processor_; }
    input::SDLGamepadBridge* bridge() { return bridge_; }
    composer::WakeStateController* wake() { return &wakeController_; }
    // The keep-screen-on OVERRIDE counter: an INPUT to the wake composer, not
    // its result. Nothing sets it today, so never bind UI to it.
    arch::Observable<int>& keepAwakeCount() { return shouldKeepScreenOn_; }
    // The derived wake intent, where `shouldInhibit` means the inhibitor is
    // held. Read-only; the header's streaming pill subscribes to it.
    const arch::Observable<composer::WakeState>& wakeState() const { return wakeComposer_.state(); }
    FeatureSettings* featureSettings() { return featureSettings_; }

    source::OnboardingPreferenceStore* onboardingStore() { return &onboardingStore_; }
    source::ThemePreferenceStore* themeStore() { return &themeStore_; }
    source::CrashReportingStore* crashStore() { return &crashStore_; }
    source::BackgroundPreferenceStore* backgroundStore() { return &backgroundStore_; }
    // The close policy, the tray's two commands and the window-visible fact the
    // tray item is derived from.
    composer::BackgroundCoordinator* background() { return backgroundCoordinator_; }

    // The auto-updater. The store is the reactive preference slice the Settings
    // page binds to; the coordinator owns the state machine, the timers and the
    // worker thread, and is started from start().
    source::UpdatePreferenceStore* updatePreferenceStore() { return &updatePrefs_; }
    update::UpdateChecker* updates() { return &updateChecker_; }

    repository::DeadzoneRepository* deadzoneRepository() { return &deadzoneRepo_; }
    source::MotionEnabledStore* motionEnabledStore() { return &motionEnabledStore_; }
    source::TouchpadModeStore* touchpadModeStore() { return &touchpadModeStore_; }
    source::JoystickRemapStore* joystickRemapStore() { return &joystickRemapStore_; }

    // The stored override if any, else the default layout.
    input::JoystickRemap remapFor(int vendorId, int productId) const;

    // Both republish, which pushes the whole set into the bridge so the change
    // takes effect live.
    void setJoystickRemap(int vendorId, int productId, const input::JoystickRemap& remap);
    void clearJoystickRemap(int vendorId, int productId);

    // While capture is on, the bridge emits rawJoystickInput, which AppModel
    // re-emits.
    void setInputCaptureEnabled(bool enabled);

    QList<input::SDLGamepadBridge::Device> attachedDevices() const { return bridge_->devices(); }

    // Runs on the Qt main thread, never per input event: the settings page calls
    // it on a slider change so the hot path picks the profile up without a
    // re-attach. Persistence is the page's job.
    void applyDeadzones(const QString& deviceId, const input::deadzone::Deadzones& dz);

    source::ControllerTypeStore* typeStore() { return &typeStore_; }

    // Empty if the slot is unbound or no catalog has landed yet.
    QList<composer::PickableType> pickableTypesFor(const QString& slotId) const;

    // Keyed on the CONNECTION instead of an existing binding: the wizard and the
    // bind-mode editor pick a type for a pad that is not bound yet, so the
    // slot-keyed read above would always vend nothing.
    QList<composer::PickableType> pickableTypesForConnection(const QString& connId) const;

    // The user override if set, else the bound catalog's first offered type,
    // else Xbox. Pre-selects the picker.
    int currentTypeFor(const QString& slotId) const;

    // The type a slot WOULD start on against a connection it is not bound to.
    int currentTypeForConnection(const QString& connId, const QString& slotId) const;

    void setSlotControllerType(const QString& slotId, int type);

    // Best-effort unauthenticated GET with ETag revalidation. Drives
    // catalogState() through Loading -> Success/Error, emitting
    // catalogStateChanged() at each transition.
    void refreshCatalogForSlot(const QString& slotId);
    void refreshCatalogForConnection(const QString& connId);

    // `hostId` is the stable satellite id, which is also the connection id and
    // the catalog cache key. All three below are synchronous peeks that never
    // trigger a fetch.

    // False makes every capability row read `Pending`: an unread layer must
    // never draw a cross.
    bool hasCatalogFor(const QString& hostId) const;

    // BY VALUE: the repository hands out a copy of its cache under its own lock,
    // so a pointer into it would dangle the moment a refresh landed.
    std::optional<models::CatalogTypeDto> catalogTypeFor(const QString& hostId, int type) const;

    QHash<QString, models::CatalogHostFeatureDto> catalogHostFeatures(const QString& hostId) const;

    // Holds the last good catalog as stale across a refresh, so the picker can
    // show a spinner or an error cause over the last-known types. Main thread.
    const source::CatalogState& catalogState() const { return catalogState_; }

    // The UI reads everything off this slice and re-renders on stateChanged().
    const MainUiState& state() const { return state_; }

    // pairingTarget is a one-shot trigger: the dialog reads it on stateChanged()
    // and clears it before showing the prompt.
    void clearPairingTarget();

    void start();

    // Null only if the gateway failed to construct, which never happens on
    // Linux. The lifecycle is owned here.
    source::usb::UsbGamepadManager* usbManager() { return usbManager_.get(); }

  signals:
    // Emitted after any field of state() changes.
    void stateChanged();

    // Transient one-shot: errors are events, not state.
    void errorMessage(const QString& msg);

    void catalogStateChanged();

    // Re-emit of the bridge's rawJoystickInput. `kind` is 0=axis/1=button/2=hat;
    // `value` the axis int16, 1 for a button, or an SDL_HAT_* bitmask.
    void rawJoystickInput(const QString& deviceId, int kind, int index, int value);

    // A forward-PIN submit was rejected. `reasonToken` is one of "wrongPin" /
    // "versionMismatch" / "unreachable" / "pending". Forwarded verbatim so the
    // sheet can match on its own target.
    void pairingFailed(const QString& connectionId, const QString& reasonToken);

  private:
    void rebuild();
    void onHubChanged();
    void onBridgeDevicesChanged();
    void onWifiEvent(const net::ConnectionEvent& evt);
    // Called from UsbObserver on the Qt main thread, the only FSM-mutating one.
    void onUsbNotice(const reducer::UsbController& c, reducer::UsbNotice notice);
    // Main thread only — it mutates the FSM.
    void pollUsbDirect();
    void onUsbDirectChanged();
    // Diff the SDL device list against the last-seen set and feed framework
    // up/down per VID:PID into the USB FSM, so a claim-failure or Standard pick
    // can settle on the live SDL device.
    void syncFrameworkPresence();
    // The SDL twin never leaves bridge_->devices() across a Direct
    // claim, so syncFrameworkPresence's appearance-edge diff never re-fires
    // after a Direct->Standard release. This drives the settle level-triggered
    // instead. Idempotent; invoked queued from onUsbDirectChanged so it cannot
    // re-enter the FSM mid effect-dispatch.
    void settleAwaitingFrameworkControllers();
    // Idempotent and off the hot path. Also called on device-add, so a freshly
    // attached pad with a saved profile decodes correctly from its first report.
    void pushJoystickRemapsToBridge();

    // Idempotent: invoked on every poolChanged so new connections get wired.
    void installRumbleHandlers();

    void syncInputRateDevices();
    // Emits stateChanged() only when a visible number moved, so a quiet 1 Hz
    // tick doesn't thrash the UI.
    void onInputRatesChanged(const source::SlotInputRatesMap& rates);

    // The user's Emulate override wins; absent that, the type whose catalog
    // `emulates` hint matches the pad's own USB identity; absent that, the bound
    // catalog's first offered type; absent that, Xbox. bind()/attachSlot thread
    // the result into the descriptor PUT.
    int resolveControllerType(const QString& slotId) const;

    // Looks in the shown slots first, then the SDL device list (a framework twin
    // hidden by an active claim is still enumerated), then the synthetic slot id
    // itself, which packs its own vid/pid. nullopt means nothing on this machine
    // accounts for the slot — the pad is gone.
    std::optional<std::pair<int, int>> boundPadIdentity(const QString& slotId) const;

    // Per-slot hardware truth read from the source layer that owns the slot:
    // the parser family for a synthetic (USB-direct) id, the SDL probe for a
    // framework id. The bind capability seams read through this so a Direct
    // claim advertises the motion/touchpad it decodes — and never the rumble/
    // lightbar it cannot drive.
    struct SlotHardware {
        bool usbDirect = false;
        bool hasMotion = false;
        bool hasLightbar = false;
        bool hasTouchpad = false;
        bool hasRumble = false;
    };
    SlotHardware slotHardware(const QString& slotId) const;

    // Warm the catalog cache once each time a satellite link goes Live, so the
    // type picker usually resolves instantly from cache. Silent by design: it
    // never drives catalogState_, so no UI spinner flickers on reconnects.
    void prewarmCatalogs();

    // Drop a binding whose physical pad vanished — otherwise its descriptor
    // rides every session PUT and the satellite keeps re-plugging a virtual
    // controller that does not exist — and migrate one whose pad merely moved
    // between its framework and USB-direct twin ids. Called at the END of
    // rebuild(); the hub's changed() re-enters rebuild, which the in-flight
    // guard makes a no-op. A drop is announced on errorMessage: the gate acts on
    // the user's behalf, so it owes them the reason.
    void applyBindingPresence();

    std::unique_ptr<net::ConnectionStore> store_;
    net::WifiConnectionManager* wifi_;
    net::ConnectionHub* hub_;
    composer::ConnectionCoordinator* connections_;
    input::GamepadInputProcessor processor_;
    input::SDLGamepadBridge* bridge_;
    FeatureSettings* featureSettings_;
    QTimer* autoReconnectTimer_;

    // WifiConnections live until teardown, so this set is never pruned.
    QSet<QString> rumbleWiredConnections_;
    // unique_ptr so tests can swap in a fake. WakeStateController holds a raw
    // back-pointer, so the lifetime must stay tied to the AppModel.
    std::unique_ptr<util::DisplaySleepInhibitor> inhibitor_;
    // Declaration order matters: the Composer captures the Observables and the
    // Controller captures the Composer's state, so both must precede it.
    arch::Observable<int> streamingSlotCount_{0};
    arch::Observable<int> shouldKeepScreenOn_{0};
    composer::WakeStateComposer wakeComposer_;
    composer::WakeStateController wakeController_;

    // Declaration order matters: each controller captures its store's
    // Observable, and the crash controller its backend, so all must precede.
    source::OnboardingPreferenceStore onboardingStore_;
    source::ThemePreferenceStore themeStore_;
    source::CrashReportingStore crashStore_;
    composer::NoopCrashReportingBackend crashBackend_;
    composer::ThemeController themeController_;
    composer::CrashReportingController crashController_;

    // Declaration order matters: the coordinators borrow the store, the tray and
    // the notifier, and the tray composer captures the coordinator's Observable.
    // unique_ptr on the three platform edges so a headless build can hold none.
    std::unique_ptr<source::TrayIcon> tray_;
    std::unique_ptr<source::DesktopNotifier> notifier_;
    std::unique_ptr<source::SleepMonitor> sleepMonitor_;
    source::BackgroundPreferenceStore backgroundStore_;
    composer::BackgroundCoordinator* backgroundCoordinator_;
    composer::SleepCoordinator* sleepCoordinator_;
    composer::TrayComposer trayComposer_;
    composer::TrayController trayController_;

    // Declaration order matters: the coordinator subscribes to the store's
    // Observable, so the store must outlive it.
    source::UpdatePreferenceStore updatePrefs_;
    update::UpdateChecker updateChecker_;

    MainUiState state_;

    // A dedicated HTTPClient for the unauthenticated catalog GET, kept off the
    // session path so a catalog fetch never perturbs a live connection.
    net::HTTPClient* catalogHttp_;
    source::SatelliteCatalogRepository catalogRepo_;
    source::ControllerTypeStore typeStore_;

    // Declaration order: the motion-preference repo must exist before the
    // MotionEnabledStore that hydrates from it.
    repository::DeadzoneRepository deadzoneRepo_;
    repository::MotionPreferenceRepository motionPrefRepo_;
    source::MotionEnabledStore motionEnabledStore_;
    // Absent = the ds4 pair-time default.
    repository::TouchpadModeRepository touchpadModeRepo_;
    source::TouchpadModeStore touchpadModeStore_{&touchpadModeRepo_};

    // Declaration order: the repo must precede the store that hydrates from it.
    // The StateSource carries the whole map per emit, so joystickRemapSub_ folds
    // each republish into a full re-push — rare, and off the hot path.
    source::JoystickRemapRepository joystickRemapRepo_;
    source::JoystickRemapStore joystickRemapStore_;
    arch::Observable<source::JoystickRemapMap>::Subscription joystickRemapSub_;
    // Device ids whose persisted deadzone profile was already pushed, so
    // onBridgeDevicesChanged pushes once per attach rather than per tick.
    QSet<QString> deadzonePushedDevices_;
    arch::Observable<composer::CatalogSnapshot> catalogSnapshot_;
    composer::CatalogComposer catalogComposer_;
    // A plain member rather than an Observable because CatalogDto has no
    // operator==. Read by the QML facade on catalogStateChanged().
    source::CatalogState catalogState_;

    // Declaration order: the gateway and store must precede the manager that
    // borrows them.
    //
    // A synthetic USB-direct device publishes decoded reports into the SAME
    // processor_ as the SDL path, keyed by its model id string, so it binds and
    // routes through the existing hub machinery. Its SDL twin is suppressed so
    // the pad streams via exactly one path.
    class UsbObserver : public source::usb::UsbDirectObserver {
      public:
        explicit UsbObserver(AppModel* owner) : owner_(owner) {}
        // One "FSM state moved" signal covers every transition, including
        // held-synthetic phase changes the granular callbacks miss.
        void controllersChanged() override { owner_->onUsbDirectChanged(); }

        // The transient "what just happened" notice. The persistent error state
        // already reaches the slot card via the controllers() snapshot.
        void notice(const reducer::UsbController& c, reducer::UsbNotice n) override {
            owner_->onUsbNotice(c, n);
        }

      private:
        AppModel* owner_;
    };

    source::UsbPathPreferenceRepository usbPathRepo_;
    source::UsbPathPreferenceStore usbPathStore_;
    std::unique_ptr<source::usb::HidrawGateway> usbGateway_;
    UsbObserver usbObserver_;
    std::unique_ptr<source::usb::UsbGamepadManager> usbManager_;
    QTimer* usbScanTimer_;
    reducer::PollRateSampler usbPollSampler_;
    // Keyed by controllers() map key, the same int the synthetic slot id is
    // built from. Main-thread-only. A synthetic that leaves Direct is pruned so
    // a reused key can't show a stale rate.
    QHash<int, int> usbPollRateHz_;
    // The VID:PIDs seen on the last syncFrameworkPresence pass, so the next can
    // emit FrameworkUp/Down deltas to the FSM. Main-thread-only.
    QSet<int> lastFrameworkVpKeys_;

    // Satellite ids already catalog-warmed for their current Live stretch;
    // reducer::catalogPrewarmTargets re-arms an id when its link leaves Live.
    // Main-thread-only.
    std::set<QString> prewarmedCatalogs_;

    // The presence oracle both the binding-presence gate and the emulation-type
    // seed read. Refilled at the top of every rebuild(), so it is never staler
    // than state_.slotList. Main-thread-only.
    std::vector<reducer::PresentSlot> presentPads_;
    // Re-entrancy guard: hub_->bind/unbind emit changed(), which lands back in
    // rebuild(). The nested pass rebuilds state normally but skips the gate, so
    // the actions are applied exactly once.
    bool bindingPresenceInFlight_ = false;

    // Built in the ctor body because its CounterSource borrows processor_. The
    // liveRatesBySlot_ cache survives slot-list rebuilds, so a rebuild triggered
    // by an unrelated change keeps the last measured numbers.
    std::unique_ptr<source::InputRateStore> inputRateStore_;
    arch::Observable<source::SlotInputRatesMap>::Subscription inputRatesSub_;
    QTimer* inputRateTimer_;
    // Carries the SDL/HID stream rates only; directPollHz is filled separately
    // from usbPollRateHz_ at rebuild(). Main-thread-only.
    QHash<QString, models::SlotLiveRates> liveRatesBySlot_;

    // slotId -> active sender. Read on the SDL gamepad thread, written on the Qt
    // main thread; routingMtx_ guards both directions.
    mutable std::mutex routingMtx_;
    QHash<QString, net::ConnectionHub::ReportSender> routing_;
    // Read on the SDL sensor / battery-poll threads, both inside
    // SDLGamepadBridge::runLoop, under the same routingMtx_.
    QHash<QString, net::ConnectionHub::MotionSender> motionRouting_;
    QHash<QString, net::ConnectionHub::BatterySender> batteryRouting_;
    QHash<QString, net::ConnectionHub::TouchpadSender> touchpadRouting_;
};

} // namespace dish
