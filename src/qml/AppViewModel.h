// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The single QML-facing adapter over dish::AppModel: it derives nothing, it
// only maps stores and composers onto Q_PROPERTY + role models. It lives in
// dish_core so the mapping helpers stay testable without the Quick stack, and
// the QmlEntryPoint registers it as a context property outliving the engine.

#pragma once

#include "Models/Models.h"
#include "architecture/Observable.h"
#include "composer/WakeStateComposer.h"
#include "core/reducer/ApplyBindingMachine.h"
#include "core/reducer/UpdateMachine.h"
#include "qml/ConnectionListModel.h"
#include "qml/SlotListModel.h"
#include "source/store/BackgroundPreferenceStore.h"
#include "source/store/CrashReportingStore.h"
#include "source/store/OnboardingPreferenceStore.h"
#include "source/store/ThemePreferenceStore.h"
#include "source/store/UiPreferenceStore.h"
#include "source/store/UpdatePreferenceStore.h"

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QTimer;

namespace dish {
class AppModel;
}

namespace dish::qml {

class AppViewModel : public QObject {
    Q_OBJECT

    // ── Dashboard header ─────────────────────────────────────────────────────
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY stateChanged)
    Q_PROPERTY(int onlineCount READ onlineCount NOTIFY stateChanged)
    Q_PROPERTY(int connectionCount READ connectionCount NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)

    // ── Live telemetry footer, sampled ~1 Hz off the processor ───────────────
    Q_PROPERTY(int eventsPerSec READ eventsPerSec NOTIFY telemetryChanged)
    Q_PROPERTY(int sendsPerSec READ sendsPerSec NOTIFY telemetryChanged)
    Q_PROPERTY(qulonglong totalSent READ totalSent NOTIFY telemetryChanged)

    // ── Pairing one-shot ─────────────────────────────────────────────────────
    Q_PROPERTY(bool pairingActive READ pairingActive NOTIFY stateChanged)
    Q_PROPERTY(QString pairingServerName READ pairingServerName NOTIFY stateChanged)
    // The parked target's stable id, so the sheet can drive both pairing paths
    // for a satellite that demanded pairing mid-connect, not only clicked rows.
    Q_PROPERTY(QString pairingServerId READ pairingServerId NOTIFY stateChanged)

    // ── Collections the pages iterate ────────────────────────────────────────
    // Owned children with pointers stable for the app lifetime, so QML binds
    // once. Not named `slots`: moc strips that token as the Q_SLOTS keyword.
    Q_PROPERTY(dish::qml::SlotListModel* slotModel READ slotModel CONSTANT)
    Q_PROPERTY(dish::qml::ConnectionListModel* connectionModel READ connectionModel CONSTANT)

    // ── Settings: appearance + diagnostics ───────────────────────────────────
    // themeMode 0=Light 1=Dark 2=System, the SettingsPage chip order.
    Q_PROPERTY(int themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(bool crashReportingEnabled READ crashReportingEnabled WRITE setCrashReportingEnabled
                   NOTIFY crashReportingChanged)
    // True when the last run left a backtrace on disk. Drives the whole report
    // card: no crash, no card, so the UI is silent in the ordinary case.
    Q_PROPERTY(bool hasCrashReport READ hasCrashReport NOTIFY crashReportChanged)

    // ── Settings: window ──────────────────────────────────────────────────────
    Q_PROPERTY(bool runInBackground READ runInBackground WRITE setRunInBackground NOTIFY
                   runInBackgroundChanged)
    // False means no StatusNotifier host is there to draw the item, so the
    // preference above cannot take effect and the row says so.
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)

    // ── About ─────────────────────────────────────────────────────────────────
    // CMake project VERSION, threaded in as DISH_VERSION.
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

    // ── Connections discovery ────────────────────────────────────────────────
    Q_PROPERTY(QVariantList discoveredServers READ discoveredServers NOTIFY discoveredChanged)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged)

    // ── Reverse (host-initiated) pairing ─────────────────────────────────────
    // Phase is "idle" | "awaiting" | "approved" | "declined" | "timedout".
    Q_PROPERTY(QString reversePairingPhase READ reversePairingPhase NOTIFY reversePairingChanged)
    Q_PROPERTY(QString reversePairingPin READ reversePairingPin NOTIFY reversePairingChanged)
    Q_PROPERTY(
        QString reversePairingServerName READ reversePairingServerName NOTIFY reversePairingChanged)

    // ── Emulate picker catalog lifecycle (AsyncState<CatalogDto>) ─────────────
    // emulateLoading is the in-flight GET with nothing cached yet; once content
    // exists a revalidate shows the stale rows instead of a blank spinner.
    Q_PROPERTY(bool emulateLoading READ emulateLoading NOTIFY emulateStateChanged)
    Q_PROPERTY(QString emulateError READ emulateError NOTIFY emulateStateChanged)
    Q_PROPERTY(bool emulateStale READ emulateStale NOTIFY emulateStateChanged)

    // ── First-run onboarding ─────────────────────────────────────────────────
    Q_PROPERTY(bool onboardingNeeded READ onboardingNeeded NOTIFY onboardingNeededChanged)

    // ── Donate ───────────────────────────────────────────────────────────────
    Q_PROPERTY(QString donateSponsorsUrl READ donateSponsorsUrl CONSTANT)
    Q_PROPERTY(QString donateKofiUrl READ donateKofiUrl CONSTANT)
    Q_PROPERTY(QString donateBmacUrl READ donateBmacUrl CONSTANT)

    // ── Shell header + rail ──────────────────────────────────────────────────
    // Counts only: the header sub-lines are assembled in QML so their words stay
    // in the qsTr catalogues.
    Q_PROPERTY(int slotCount READ slotCount NOTIFY stateChanged)
    Q_PROPERTY(int boundSlotCount READ boundSlotCount NOTIFY stateChanged)
    // Bound and the link Connected, the same composer::streamingSlotCount rule
    // the wake controller inhibits the display on.
    Q_PROPERTY(int streamingSlotCount READ streamingSlotCount NOTIFY stateChanged)
    Q_PROPERTY(QString firstOnlineName READ firstOnlineName NOTIFY stateChanged)
    Q_PROPERTY(int foundCount READ foundCount NOTIFY discoveredChanged)
    // How far the hold currently reaches: "off" | "system" | "display".
    Q_PROPERTY(QString keepAwakeReach READ keepAwakeReach NOTIFY stateChanged)
    Q_PROPERTY(
        bool railCollapsed READ railCollapsed WRITE setRailCollapsed NOTIFY railCollapsedChanged)
    Q_PROPERTY(bool lightbarFollowGame READ lightbarFollowGame WRITE setLightbarFollowGame NOTIFY
                   lightbarChanged)

    // ── Settings: keep awake ─────────────────────────────────────────────────
    // 0 Off, 1 While a controller is active, 2 While connected — the
    // SettingsPage option order.
    Q_PROPERTY(
        int keepAwakeMode READ keepAwakeMode WRITE setKeepAwakeMode NOTIFY keepAwakePrefsChanged)
    // Minutes of stillness before mode 1 lets go. Clamped 1..180 by the store.
    Q_PROPERTY(int keepAwakeTimeoutMinutes READ keepAwakeTimeoutMinutes WRITE
                   setKeepAwakeTimeoutMinutes NOTIFY keepAwakePrefsChanged)
    // Whether the hold covers the screen as well as the machine.
    Q_PROPERTY(bool keepDisplayAwake READ keepDisplayAwake WRITE setKeepDisplayAwake NOTIFY
                   keepAwakePrefsChanged)

    // ── Bluetooth radio ──────────────────────────────────────────────────────
    // Two facts, not one: an absent adapter and a switched-off radio need
    // different copy, and only the second has an action.
    Q_PROPERTY(bool bluetoothPresent READ bluetoothPresent NOTIFY bluetoothChanged)
    Q_PROPERTY(bool bluetoothEnabled READ bluetoothEnabled NOTIFY bluetoothChanged)

    // ── Apply sequencer ──────────────────────────────────────────────────────
    // The Connection step can sit for seconds while the kernel hands the device over,
    // which is why it is a step with an elapsed clock and an escape.
    Q_PROPERTY(bool applyInFlight READ applyInFlight NOTIFY applyChanged)
    // "pending" | "active" | "done" | "failed" | "skipped".
    Q_PROPERTY(QString applyConnectionState READ applyConnectionState NOTIFY applyChanged)
    Q_PROPERTY(QString applyDestinationState READ applyDestinationState NOTIFY applyChanged)
    // Milliseconds on the current step.
    Q_PROPERTY(int applyElapsedMs READ applyElapsedMs NOTIFY applyChanged)
    // Only while the Connection step is active: aborting a claim falls back to
    // Standard, but the REST round-trip cannot be cancelled at all.
    Q_PROPERTY(bool applyCancellable READ applyCancellable NOTIFY applyChanged)

    // ── Auto-updater ─────────────────────────────────────────────────────────
    // Tokens only, never sentences: every word the user reads is composed in
    // QML so it stays in the qsTr catalogues.
    // "disabled"|"idle"|"checking"|"upToDate"|"available"|"failed".
    Q_PROPERTY(QString updatePhase READ updatePhase NOTIFY updateChanged)
    // The target version, "" when there is nothing to offer.
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateChanged)
    // ""|"offline"|"http"|"manifestInvalid". Empty in every phase but failed.
    Q_PROPERTY(QString updateErrorToken READ updateErrorToken NOTIFY updateChanged)
    Q_PROPERTY(QString updateNotesUrl READ updateNotesUrl NOTIFY updateChanged)
    // The running build is below the manifest's minimumSupportedVersion.
    Q_PROPERTY(bool updateRequired READ updateRequired NOTIFY updateChanged)
    // Invalid means never checked.
    Q_PROPERTY(QDateTime updateLastCheck READ updateLastCheck NOTIFY updateChanged)
    Q_PROPERTY(bool updateChecksEnabled READ updateChecksEnabled WRITE setUpdateChecksEnabled NOTIFY
                   updatePrefsChanged)

  public:
    explicit AppViewModel(dish::AppModel* model, QObject* parent = nullptr);

    QString statusText() const { return statusText_; }
    QString summaryText() const { return summaryText_; }
    int onlineCount() const { return onlineCount_; }
    int connectionCount() const { return connectionCount_; }
    bool busy() const { return busy_; }

    int eventsPerSec() const { return eventsPerSec_; }
    int sendsPerSec() const { return sendsPerSec_; }
    qulonglong totalSent() const { return totalSent_; }

    bool pairingActive() const { return pairingActive_; }
    QString pairingServerName() const { return pairingServerName_; }
    QString pairingServerId() const { return pairingServerId_; }

    SlotListModel* slotModel() { return &slotModel_; }
    ConnectionListModel* connectionModel() { return &connectionModel_; }

    int themeMode() const;
    // Q_INVOKABLE as well as the property WRITE: QML can only assign a WRITE
    // accessor, and the pages call these as functions.
    Q_INVOKABLE void setThemeMode(int mode);
    bool crashReportingEnabled() const;
    bool hasCrashReport() const;
    Q_INVOKABLE void setCrashReportingEnabled(bool enabled);

    // ── Crash report ─────────────────────────────────────────────────────────
    // The redacted text, exactly as it would be sent. Shown before any send so
    // the user approves what leaves the machine rather than trusting a promise.
    Q_INVOKABLE QString crashReportText() const;
    // Puts that same text on the clipboard.
    Q_INVOKABLE void copyCrashReport() const;
    // Opens a prefilled GitHub issue in the browser. Nothing is uploaded by Dish
    // itself: the browser is the transport and the user is the one who submits.
    Q_INVOKABLE void openCrashIssue() const;
    // Deletes the log, which is what makes the card go away.
    Q_INVOKABLE void discardCrashReport();
    bool runInBackground() const;
    Q_INVOKABLE void setRunInBackground(bool enabled);
    bool trayAvailable() const;
    // True when the shell should hide the window instead of quitting. Not a
    // query: this is what spends the one-time "still running" notice.
    Q_INVOKABLE bool requestWindowClose();
    Q_INVOKABLE void setWindowVisible(bool visible);
    QString appVersion() const;
    bool onboardingNeeded() const;
    QString reversePairingPhase() const;
    QString reversePairingPin() const;
    QString reversePairingServerName() const;
    QString donateSponsorsUrl() const;
    QString donateKofiUrl() const;
    QString donateBmacUrl() const;

    int slotCount() const { return slotCount_; }
    int boundSlotCount() const { return boundSlotCount_; }
    int streamingSlotCount() const { return streamingSlotCount_; }
    QString firstOnlineName() const { return firstOnlineName_; }
    int foundCount() const;
    QString keepAwakeReach() const { return keepAwakeReach_; }
    bool railCollapsed() const;
    Q_INVOKABLE void setRailCollapsed(bool collapsed);
    bool lightbarFollowGame() const;
    Q_INVOKABLE void setLightbarFollowGame(bool followGame);

    int keepAwakeMode() const;
    Q_INVOKABLE void setKeepAwakeMode(int mode);
    int keepAwakeTimeoutMinutes() const;
    Q_INVOKABLE void setKeepAwakeTimeoutMinutes(int minutes);
    bool keepDisplayAwake() const;
    Q_INVOKABLE void setKeepDisplayAwake(bool enabled);

    bool bluetoothPresent() const { return bluetoothPresent_; }
    bool bluetoothEnabled() const { return bluetoothEnabled_; }
    bool applyInFlight() const;
    QString applyConnectionState() const;
    QString applyDestinationState() const;
    int applyElapsedMs() const { return apply_.elapsedMsOnStep; }
    bool applyCancellable() const;

    // A seam so dish_core need not link the exe-only ExternalLink and
    // NotificationQueue. Returns true when the open was handed off; unset in
    // tests, where the default falls back to QDesktopServices.
    using ExternalOpenSink = std::function<bool(const QString& url)>;
    void setExternalOpenSink(ExternalOpenSink sink) { externalOpenSink_ = std::move(sink); }

    // Runs after setThemeMode so the QML palette and the native title bar follow
    // the new mode (true == dark). Unset in tests.
    using ThemeAppliedSink = std::function<void(bool dark)>;
    void setThemeAppliedSink(ThemeAppliedSink sink) { themeAppliedSink_ = std::move(sink); }

    // ── Commands, forwarded verbatim to the AppModel surface ────────────────

    Q_INVOKABLE void bindSlot(const QString& slotId, const QString& connectionId);
    Q_INVOKABLE void unbindSlot(const QString& slotId);

    // "standard" (SDL owns the pad), "direct" (raw-HID claim) or "auto"
    // (clear the override). Resolving slotId to (vid, pid) differs per source: a
    // synthetic slot's id IS the packed vpKey, an SDL slot's identity comes from
    // the bridge device list. A no-op when neither resolves.
    Q_INVOKABLE void setSlotPath(const QString& slotId, const QString& choice);

    // Rows of {connectionId,label,dotColor,glyph} from the pure
    // reducer::connectionsVisibleInPicker. One-shot, read when the chooser
    // opens, so no NOTIFY.
    Q_INVOKABLE QVariantList availableConnectionsForSlot(const QString& slotId) const;
    // The pads riding `connectionId`, as {name, emulateName} rows — the manifest
    // a Forget confirm shows before it drops them. Here rather than joined in
    // QML off an invisible Repeater, which re-derives domain state in the view
    // and is unresolvable to qmllint.
    Q_INVOKABLE QVariantList carriedPads(const QString& connectionId) const;
    // 1-based position of `slotId` among the pads riding `connectionId`, 0 when
    // it rides none. Same join as carriedPads, so the two always agree.
    Q_INVOKABLE int slotOrdinal(const QString& slotId, const QString& connectionId) const;

    // emulateTypes rows are {type,slug,name,shortName,description,known};
    // emulateCurrentType is the pre-selected wire id.
    Q_INVOKABLE void refreshEmulate(const QString& slotId);
    Q_INVOKABLE QVariantList emulateTypes(const QString& slotId) const;
    Q_INVOKABLE int emulateCurrentType(const QString& slotId) const;
    Q_INVOKABLE void setControllerType(const QString& slotId, int type);

    // The same three keyed on the destination rather than an existing binding:
    // the wizard picks a type for a pad that is not bound yet, which the
    // slot-keyed reads above vend nothing for.
    Q_INVOKABLE void refreshEmulateForHost(const QString& connectionId);
    Q_INVOKABLE QVariantList emulateTypesForHost(const QString& connectionId) const;
    Q_INVOKABLE int emulateCurrentTypeForHost(const QString& connectionId,
                                              const QString& slotId) const;
    bool emulateLoading() const;
    QString emulateError() const;
    bool emulateStale() const;

    // discoveredServers rows are
    // {name,ip,udpPort,pairPort,httpPort,machineId,source,id}.
    Q_INVOKABLE void startDiscovery();
    Q_INVOKABLE bool isScanning() const;
    Q_INVOKABLE QVariantList discoveredServers() const;
    Q_INVOKABLE void forgetConnection(const QString& connectionId);

    // Keyed on the stable id, never a list index: the discovered list can reorder
    // between the read and the call.
    Q_INVOKABLE void connectByServerId(const QString& serverId);

    // Reconnects a remembered satellite that may not be discovered right now,
    // with no rescan and no re-pairing.
    Q_INVOKABLE void reconnectConnection(const QString& connectionId);

    // Disconnects a live session without forgetting it: the row and the key stay.
    Q_INVOKABLE void disconnectConnection(const QString& connectionId);

    // clearPairingTarget consumes the one-shot trigger before the sheet shows.
    Q_INVOKABLE void pairByServerId(const QString& serverId, const QString& pin);
    Q_INVOKABLE bool isPairingInFlight(const QString& serverId) const;
    Q_INVOKABLE void clearPairingTarget();

    Q_INVOKABLE void requestReversePairing(const QString& serverId);
    Q_INVOKABLE void cancelReversePairing();

    // ── Deadzone settings page ───────────────────────────────────────────────
    // Rows of {id,name,hasGyro,stickFlat,triggerFlat,forwardMotion}, re-pulled on
    // deadzonesChanged. setDeadzones both persists the override and pushes it
    // into the live processor.
    Q_INVOKABLE QVariantList deadzoneDevices() const;
    Q_INVOKABLE void setDeadzones(const QString& deviceId, int stickFlat, int triggerFlat);
    Q_INVOKABLE void setMotionEnabled(const QString& deviceId, bool enabled);

    // ── Configure-controls (raw-joystick remap) page ─────────────────────────
    // slotRemap is the EFFECTIVE remap: which raw axis/button/hat each logical
    // output reads, plus the invert states. Persisting an assignment pushes it
    // into the bridge, so it takes effect live. All resolve slotId to (vid,pid)
    // the way setSlotPath does, and no-op when that fails.
    Q_INVOKABLE QVariantMap slotRemap(const QString& slotId) const;
    Q_INVOKABLE void assignSlotInput(const QString& slotId, const QString& target, int kind,
                                     int index);
    Q_INVOKABLE void setSlotInvert(const QString& slotId, const QString& which, bool on);
    Q_INVOKABLE void resetSlotRemap(const QString& slotId);

    // startInputCapture also records which slot is capturing, so the
    // rawInputCaptured relay can filter to it.
    Q_INVOKABLE void startInputCapture(const QString& slotId);
    Q_INVOKABLE void stopInputCapture();

    // ── Bluetooth radio ──────────────────────────────────────────────────────
    // Emits only on a real change.
    Q_INVOKABLE void refreshBluetoothState();
    Q_INVOKABLE void openBluetoothSettings();

    // ── The capability surface ───────────────────────────────────────────────
    // One row per feature in fixed render order:
    // { feature, inOk, linkOk, typeOk, hostOk, verdict, failingLayer,
    //   hasFailingLayer }. feature / verdict / failingLayer are lowercase tokens
    // ("motion", "unavailable", "link"): the C++ never vends a sentence, QML
    // localizes. hostKind is "satellite" or "bluetooth", hostId "" means no
    // destination chosen yet, desiredPath is "standard" or "direct" and
    // touchpadMode is 0=off 1=pad 2=mouse.
    Q_INVOKABLE QVariantList capabilityForCandidate(const QString& slotId, int type,
                                                    const QString& hostKind, const QString& hostId,
                                                    const QString& desiredPath, bool motionOn,
                                                    bool rumbleOn, int touchpadMode) const;

    // { feature, supported } rows for the type picker's preview pills. Empty
    // while the catalog is unresolved: a guessed pill is worse than none.
    Q_INVOKABLE QVariantList typeFeatureSummary(const QString& hostId, int type) const;

    // False means every capability row renders Pending, never a cross.
    Q_INVOKABLE bool catalogResolvedFor(const QString& hostId) const;

    // ── Host slot accounting ─────────────────────────────────────────────────
    // Counts only. A slot NUMBER must never be asserted before bindSlot
    // allocates one.
    Q_INVOKABLE int hostBoundSlotCount(const QString& connectionId) const;
    Q_INVOKABLE int hostSlotCapacity() const;
    // The pad that would be unbound if this host is already full, "" when none.
    Q_INVOKABLE QString displacedSlotName(const QString& connectionId) const;

    // ── Binding-draft helpers ───────────────────────────────────────────────
    // Re-resolve by (vid, pid) immediately before binding: a Direct claim retires
    // the framework slot id for a synthetic twin, so the id the wizard opened
    // with can already be stale. "" when the pad is genuinely gone.
    Q_INVOKABLE QString resolveSlotIdForBind(const QString& slotId) const;
    // Whether the raw-HID fast lane knows this model's report layout. Always
    // false over Bluetooth.
    Q_INVOKABLE bool isVerifiedModel(const QString& slotId) const;
    // "off" | "pad" | "mouse", "off" when the user never picked one: the resolve
    // ladder owns any richer default.
    Q_INVOKABLE QString touchpadModeFor(const QString& connectionId) const;
    Q_INVOKABLE void setTouchpadMode(const QString& connectionId, const QString& mode);
    // Keyed exactly as setMotionEnabled writes, so a draft seeded from it cannot
    // silently re-enable gyro the user turned off on the Dead zones page.
    Q_INVOKABLE bool motionEnabledFor(const QString& slotId) const;
    Q_INVOKABLE bool rumbleEnabledFor(const QString& slotId) const;
    Q_INVOKABLE void setRumbleEnabled(const QString& slotId, bool on);

    // ── Apply ────────────────────────────────────────────────────────────────
    // The one write the binding surfaces make. Terminates in exactly one
    // applyFinished, and is ignored while a run is already in flight.
    Q_INVOKABLE void applyBinding(const QString& slotId, const QString& connectionId, int type,
                                  const QString& desiredPath, bool motionOn, bool rumbleOn,
                                  int touchpadMode);
    // Accepted only while the Connection step is active.
    Q_INVOKABLE void cancelApply();

    // The same discovery-source label discoveredServers() vends, by id.
    Q_INVOKABLE QString discoverySourceFor(const QString& serverId) const;

    // ── Licenses page ────────────────────────────────────────────────────────
    // The bundled third-party manifest as {name,version,license,url} rows.
    Q_INVOKABLE QVariantList licenses() const;

    // ── Onboarding + external links ──────────────────────────────────────────
    Q_INVOKABLE void markOnboardingComplete();
    Q_INVOKABLE void openExternalUrl(const QString& url);

    // ── Auto-updater ─────────────────────────────────────────────────────────
    QString updatePhase() const;
    QString updateVersion() const;
    QString updateErrorToken() const;
    QString updateNotesUrl() const;
    bool updateRequired() const { return update_.required; }
    QDateTime updateLastCheck() const;
    bool updateChecksEnabled() const { return update_.checksEnabled; }
    Q_INVOKABLE void setUpdateChecksEnabled(bool enabled);

    Q_INVOKABLE void checkForUpdatesNow();
    Q_INVOKABLE void skipUpdate();
    // Falls back to the releases page when the manifest carried no notes URL.
    Q_INVOKABLE void openReleaseNotes();

  signals:
    // Folds AppModel's stateChanged and the coordinator's connectionsChanged.
    void stateChanged();
    void telemetryChanged();
    void errorMessage(const QString& message);

    void themeModeChanged();
    void crashReportingChanged();
    void crashReportChanged();
    void onboardingNeededChanged();
    void runInBackgroundChanged();
    void trayAvailableChanged();
    // Raised by the tray item's menu, which is the only way back to a hidden
    // window and the only way out of the process.
    void showWindowRequested();
    void quitRequested();

    // Folds the manager's discoveredChanged AND a connection-row id-set move:
    // the FOUND list excludes ids that already have a row (the one-spot rule),
    // so a pair landing or a forget has to re-read too.
    void discoveredChanged();

    void scanningChanged();
    void reversePairingChanged();
    void emulateStateChanged();
    void deadzonesChanged();
    void railCollapsedChanged();

    // Folds FeatureSettings::changed so an external mutation re-reads too.
    void lightbarChanged();

    void keepAwakePrefsChanged();

    // The rising edge of a connection going live after a pair. Best-effort,
    // fired at most once per live transition.
    void pairingSucceeded();

    // kind is 0=axis 1=button 2=hat; value is the axis int16, 1 for a button, or
    // an SDL_HAT_* bitmask. Re-emitted from AppModel's rawJoystickInput only when
    // the source deviceId maps to the capturing slot.
    void rawInputCaptured(const QString& slotId, int kind, int index, int value);

    void bluetoothChanged();
    void applyChanged();

    // Fired exactly once per run. reasonToken is "" on success, else "slotGone" |
    // "hostUnreachable" | "bindRejected" | "cancelled". directFellBack means the
    // Direct claim did not land and the pad streams over Standard, a warning
    // rather than an error.
    void applyFinished(bool ok, const QString& reasonToken, bool directFellBack);

    // Keyed by the stable discovered-server id the sheet opened for. reasonToken
    // is "wrongPin" | "versionMismatch" | "unreachable" | "pending". The toast
    // fires as well, and that duplication is intended.
    void pairingFailed(const QString& serverId, const QString& reasonToken);

    void updateChanged();
    void updatePrefsChanged();

    // token is "available" | "unsupported"; edge-detected in the coordinator, so
    // it fires once per version per session. Periodic check FAILURES never reach
    // here: they live in Settings.
    void updateNotice(const QString& token, const QString& version);

  private:
    void onStateChanged();
    void onConnectionsChanged();
    void onTelemetryTick();
    void onRawJoystickInput(const QString& deviceId, int kind, int index, int value);

    // ── Apply sequencer internals ────────────────────────────────────────────
    // Feeds the pure machine, stops the budgets it has outgrown, and fires
    // applyFinished once on reaching a terminal phase.
    void dispatchApply(const reducer::ApplyEvent& event);
    void beginApplyBind();
    void onApplyTick();
    const models::ControllerSlot* slotById(const QString& slotId) const;

    dish::AppModel* model_;
    SlotListModel slotModel_;
    ConnectionListModel connectionModel_;
    QTimer* telemetryTimer_;

    QString statusText_;
    QString summaryText_;
    int onlineCount_ = 0;
    int connectionCount_ = 0;
    bool busy_ = false;
    int slotCount_ = 0;
    int boundSlotCount_ = 0;
    int streamingSlotCount_ = 0;
    QString firstOnlineName_;
    QString keepAwakeReach_ = QStringLiteral("off");

    int eventsPerSec_ = 0;
    int sendsPerSec_ = 0;
    qulonglong totalSent_ = 0;

    bool pairingActive_ = false;
    QString pairingServerName_;
    QString pairingServerId_;

    // Empty when no capture is running. The rawJoystickInput relay filters to it
    // so only the capturing slot's page sees the event.
    QString capturingSlotId_;

    // Edge detector for pairingSucceeded().
    int lastOnlineCount_ = 0;

    // Edge detector only: discoveredServers() re-reads the coordinator's rows on
    // every call, never this cache. A membership move re-emits discoveredChanged
    // because the FOUND list excludes ids that already have a row.
    QSet<QString> connectionRowIds_;

    // Edge detector, so onboardingNeededChanged fires only on a real transition.
    bool onboardingNeeded_ = false;

    // The probe enumerates SetupAPI, so it is sampled on demand rather than on
    // every binding read.
    bool bluetoothPresent_ = false;
    bool bluetoothEnabled_ = false;

    // ── Apply sequencer state ────────────────────────────────────────────────
    // The run's parameters are held so the Connection step can hand off to the
    // Destination step without the caller re-supplying them.
    reducer::ApplyState apply_;
    QString applySlotId_;
    QString applyConnectionId_;
    int applyType_ = 0;
    bool applyMotionOn_ = true;
    bool applyRumbleOn_ = true;
    int applyTouchpadMode_ = 0;
    // The 20 s path budget, the 8 s bind budget, and the 250 ms elapsed tick.
    QTimer* applyPathTimer_ = nullptr;
    QTimer* applyBindTimer_ = nullptr;
    QTimer* applyTickTimer_ = nullptr;

    ExternalOpenSink externalOpenSink_;
    ThemeAppliedSink themeAppliedSink_;

    // Held for the model's lifetime: dropping a subscription stops the store-side
    // republish from reaching the Qt NOTIFYs.
    arch::Observable<source::ThemeMode>::Subscription themeSub_;
    arch::Observable<bool>::Subscription crashSub_;
    arch::Observable<source::OnboardingState>::Subscription onboardingSub_;
    arch::Observable<composer::WakeState>::Subscription keepAwakeSub_;
    arch::Observable<reducer::KeepAwakePreferences>::Subscription keepAwakePrefsSub_;
    arch::Observable<source::BackgroundPreferences>::Subscription backgroundSub_;

    // The updater's whole surface is this one cached slice, republished by the
    // coordinator; the prefs subscription only exists so an external write
    // (the boot gate's quarantine mute) still reaches the toggles.
    reducer::UpdateStatus update_;
    arch::Observable<reducer::UpdateStatus>::Subscription updateSub_;
    arch::Observable<source::UpdatePreferences>::Subscription updatePrefsSub_;

    // Shell-only state, so the store lives here rather than on AppModel.
    source::UiPreferenceStore uiPrefs_;
};

} // namespace dish::qml
