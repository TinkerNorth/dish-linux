// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/AppViewModel.h"

#include "AppModel.h"
#include "FeatureSettings.h"
#include "Input/GamepadInputProcessor.h"
#include "Input/JoystickMapping.h"
#include "Input/SDLGamepadBridge.h"
#include "Network/WifiConnectionManager.h"
#include "composer/CatalogComposer.h"
#include "composer/ConnectionCoordinator.h"
#include "composer/StreamingSlotCount.h"
#include "UI/CrashReport.h"
#include "core/catalog/BundledCatalog.h"
#include "core/input/Deadzones.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightSessionUi.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightManager.h"
#include "core/reducer/CapabilitySolver.h"
#include "core/reducer/CatalogFeatureGate.h"
#include "core/reducer/CarriedPads.h"
#include "core/reducer/ConnectionRows.h"
#include "core/reducer/FoundVisibility.h"
#include "core/reducer/PathChoice.h"
#include "core/reducer/PickerVisibility.h"
#include "core/reducer/SlotPathFields.h"
#include "source/usb/UsbGamepadManager.h"
#include "qml/AppSettingsMaps.h"
#include "repository/DeadzoneRepository.h"
#include "source/store/CrashReportingStore.h"
#include "source/store/MotionEnabledStore.h"
#include "source/store/OnboardingPreferenceStore.h"
#include "source/store/ControllerTypeStore.h"
#include "source/store/ThemePreferenceStore.h"
#include "source/store/TouchpadModeStore.h"
#include "UI/Theme.h"
#include "UI/licenses/LicenseManifest.h"

#include <QDateTime>
#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSet>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <map>
#include <optional>

#include "source/system/BluetoothRadioProbe.h"

#ifndef DISH_VERSION
#define DISH_VERSION "0.0.0"
#endif

namespace dish::qml {

namespace {

// reducer::UiLinkState is the Qt-free mirror of models::LinkState: same seven
// values in the same order, so the cast is the crossing.
reducer::UiLinkState toUiLinkState(models::LinkState s) {
    return static_cast<reducer::UiLinkState>(s);
}

// The tokens ConnectionListModel exposes, so a bind-chooser row and a
// Connections-page row cannot render differently.
QString dotColorToken(models::LinkState s) {
    switch (reducer::dotColorForState(toUiLinkState(s))) {
    case reducer::DotColor::Success:
        return QStringLiteral("success");
    case reducer::DotColor::Primary:
        return QStringLiteral("primary");
    case reducer::DotColor::Warning:
        return QStringLiteral("warning");
    case reducer::DotColor::Muted:
        return QStringLiteral("muted");
    }
    return {};
}

QString glyphToken(models::LinkState s) {
    switch (reducer::glyphForConnection(reducer::ConnectionKind::Satellite, toUiLinkState(s))) {
    case reducer::ConnectionGlyph::SatelliteBase:
        return QStringLiteral("satelliteBase");
    case reducer::ConnectionGlyph::SatelliteConnected:
        return QStringLiteral("satelliteConnected");
    case reducer::ConnectionGlyph::SatelliteOff:
        return QStringLiteral("satelliteOff");
    }
    return {};
}

// ── Configure-controls token maps ────────────────────────────────────────────
// The strings match the JoystickRemap field names. An unknown one returns
// nullopt so the caller no-ops, letting a newer page name a target this build
// does not have.
std::optional<input::RemapTarget> remapTargetFromString(const QString& t) {
    using T = input::RemapTarget;
    static const std::map<QString, T> kMap{
        {QStringLiteral("a"), T::A},
        {QStringLiteral("b"), T::B},
        {QStringLiteral("x"), T::X},
        {QStringLiteral("y"), T::Y},
        {QStringLiteral("dpadUp"), T::DpadUp},
        {QStringLiteral("dpadDown"), T::DpadDown},
        {QStringLiteral("dpadLeft"), T::DpadLeft},
        {QStringLiteral("dpadRight"), T::DpadRight},
        {QStringLiteral("leftShoulder"), T::LeftShoulder},
        {QStringLiteral("rightShoulder"), T::RightShoulder},
        {QStringLiteral("back"), T::Back},
        {QStringLiteral("start"), T::Start},
        {QStringLiteral("leftThumb"), T::LeftThumb},
        {QStringLiteral("rightThumb"), T::RightThumb},
        {QStringLiteral("leftStickX"), T::LeftStickX},
        {QStringLiteral("leftStickY"), T::LeftStickY},
        {QStringLiteral("rightStickX"), T::RightStickX},
        {QStringLiteral("rightStickY"), T::RightStickY},
        {QStringLiteral("leftTrigger"), T::LeftTrigger},
        {QStringLiteral("rightTrigger"), T::RightTrigger},
    };
    const auto it = kMap.find(t);
    if (it == kMap.end()) { return std::nullopt; }
    return it->second;
}

// {kind:"axis"|"button", index:int}
QVariantMap triggerSourceToMap(const input::TriggerSource& src) {
    QVariantMap m;
    m[QStringLiteral("kind")] = src.kind == input::TriggerSourceKind::Button
                                    ? QStringLiteral("button")
                                    : QStringLiteral("axis");
    m[QStringLiteral("index")] = src.index;
    return m;
}

// ── Capability vocabulary → QML tokens ───────────────────────────────────────
// Lowercase tokens only: every user-facing word is composed in QML so the
// strings stay in the qsTr catalogues.

QString capFeatureToken(reducer::CapFeature f) {
    switch (f) {
    case reducer::CapFeature::Gamepad:
        return QStringLiteral("gamepad");
    case reducer::CapFeature::Triggers:
        return QStringLiteral("triggers");
    case reducer::CapFeature::Motion:
        return QStringLiteral("motion");
    case reducer::CapFeature::Touchpad:
        return QStringLiteral("touchpad");
    case reducer::CapFeature::Mouse:
        return QStringLiteral("mouse");
    case reducer::CapFeature::Rumble:
        return QStringLiteral("rumble");
    case reducer::CapFeature::Lightbar:
        return QStringLiteral("lightbar");
    }
    return {};
}

QString capVerdictToken(reducer::CapVerdict v) {
    switch (v) {
    case reducer::CapVerdict::Available:
        return QStringLiteral("available");
    case reducer::CapVerdict::Unavailable:
        return QStringLiteral("unavailable");
    case reducer::CapVerdict::Pending:
        return QStringLiteral("pending");
    case reducer::CapVerdict::Off:
        return QStringLiteral("off");
    }
    return {};
}

QString capLayerToken(reducer::CapLayer l) {
    switch (l) {
    case reducer::CapLayer::Input:
        return QStringLiteral("input");
    case reducer::CapLayer::Link:
        return QStringLiteral("link");
    case reducer::CapLayer::Type:
        return QStringLiteral("type");
    case reducer::CapLayer::Host:
        return QStringLiteral("host");
    }
    return {};
}

QString applyStepToken(reducer::ApplyStepState s) {
    switch (s) {
    case reducer::ApplyStepState::Pending:
        return QStringLiteral("pending");
    case reducer::ApplyStepState::Active:
        return QStringLiteral("active");
    case reducer::ApplyStepState::Done:
        return QStringLiteral("done");
    case reducer::ApplyStepState::Failed:
        return QStringLiteral("failed");
    case reducer::ApplyStepState::Skipped:
        return QStringLiteral("skipped");
    }
    return {};
}

// PathClaimTimeout never reaches here: a claim that times out is a fallback
// (directFellBack), not a failure. The arm only keeps the switch total.
QString applyFailureToken(reducer::ApplyFailure f) {
    switch (f) {
    case reducer::ApplyFailure::SlotGone:
        return QStringLiteral("slotGone");
    case reducer::ApplyFailure::HostUnreachable:
    case reducer::ApplyFailure::PathClaimTimeout:
        return QStringLiteral("hostUnreachable");
    case reducer::ApplyFailure::BindRejected:
        return QStringLiteral("bindRejected");
    case reducer::ApplyFailure::Cancelled:
        return QStringLiteral("cancelled");
    }
    return {};
}

// ── Updater vocabulary → QML tokens ──────────────────────────────────────────

QString updatePhaseToken(reducer::UpdatePhase phase) {
    switch (phase) {
    case reducer::UpdatePhase::Disabled:
        return QStringLiteral("disabled");
    case reducer::UpdatePhase::Idle:
        return QStringLiteral("idle");
    case reducer::UpdatePhase::Checking:
        return QStringLiteral("checking");
    case reducer::UpdatePhase::UpToDate:
        return QStringLiteral("upToDate");
    case reducer::UpdatePhase::Available:
        return QStringLiteral("available");
    case reducer::UpdatePhase::Failed:
        return QStringLiteral("failed");
    }
    return {};
}

QString updateErrorTokenFor(reducer::UpdateError error) {
    switch (error) {
    case reducer::UpdateError::None:
        return {};
    case reducer::UpdateError::Offline:
        return QStringLiteral("offline");
    case reducer::UpdateError::Http:
        return QStringLiteral("http");
    case reducer::UpdateError::ManifestInvalid:
        return QStringLiteral("manifestInvalid");
    }
    return {};
}

QString updateNoticeToken(reducer::UpdateNotice notice) {
    switch (notice) {
    case reducer::UpdateNotice::Available:
        return QStringLiteral("available");
    case reducer::UpdateNotice::Unsupported:
        return QStringLiteral("unsupported");
    }
    return {};
}

// Where the popover sends the user, and the fallback when a manifest carried no
// releaseNotesUrl (the field is advisory and may be dropped by validation).
constexpr const char* kReleasesPageUrl =
    "https://github.com/TinkerNorth/dish-linux/releases/latest";

// The satellite's controller board is four pads wide.
constexpr int kHostSlotCapacity = 4;
// A raw-HID claim can hold the device while the kernel releases it; a REST
// round-trip that has not answered in 8 s means an unreachable host.
constexpr int kPathBudgetMs = 20'000;
constexpr int kBindBudgetMs = 8'000;
constexpr int kApplyTickMs = 250;

} // namespace

AppViewModel::AppViewModel(dish::AppModel* model, QObject* parent)
    : QObject(parent), model_(model), slotModel_(this), connectionModel_(this) {
    QObject::connect(model_, &dish::AppModel::stateChanged, this, &AppViewModel::onStateChanged);
    QObject::connect(model_, &dish::AppModel::errorMessage, this, &AppViewModel::errorMessage);
    QObject::connect(model_->connections(), &composer::ConnectionCoordinator::connectionsChanged,
                     this, &AppViewModel::onConnectionsChanged);
    QObject::connect(model_, &dish::AppModel::rawJoystickInput, this,
                     &AppViewModel::onRawJoystickInput);

    QObject::connect(model_->wifi(), &net::WifiConnectionManager::discoveredChanged, this,
                     [this] { emit discoveredChanged(); });
    QObject::connect(model_->wifi(), &net::WifiConnectionManager::scanningChanged, this,
                     [this] { emit scanningChanged(); });

    // The Moonlight subsystem folds all of its changes (rows, scan state,
    // pairing PIN/phase) into one moonlightChanged() the QML binds against.
    QObject::connect(model_->moonlight(), &source::moon::MoonlightManager::rowsChanged, this,
                     [this] { emit moonlightChanged(); });
    QObject::connect(model_->moonlight(), &source::moon::MoonlightManager::scanningChanged, this,
                     [this] { emit moonlightChanged(); });
    QObject::connect(model_->moonlight(), &source::moon::MoonlightManager::pairingChanged, this,
                     [this] { emit moonlightChanged(); });
    // No toast on a refusal: the binding flow and the pairing sheet both render
    // the refused state with copy that says what to do about it, and a second,
    // vaguer sentence over the top of it is noise.
    QObject::connect(model_->moonlight(), &source::moon::MoonlightManager::pairingFinished, this,
                     [this](const QString&, bool, const QString&) { emit moonlightChanged(); });
    QObject::connect(model_->moonlight(), &source::moon::MoonlightManager::probeFinished, this,
                     [this](const QString&) { emit moonlightChanged(); });
    QObject::connect(model_->moonlight(), &source::moon::MoonlightManager::appsChanged, this,
                     [this](const QString& uuid) {
                         emit moonlightAppsChanged(uuid);
                         emit moonlightChanged();
                     });
    QObject::connect(model_->wifi(), &net::WifiConnectionManager::reversePairingChanged, this,
                     [this] { emit reversePairingChanged(); });
    QObject::connect(model_, &dish::AppModel::catalogStateChanged, this,
                     [this] { emit emulateStateChanged(); });

    // The settings stores republish through StateSource Observables, not Qt
    // signals, so subscribe to turn a republish into our Qt NOTIFY.
    // emitCurrent=false: the getters read the initial values lazily.
    themeSub_ = model_->themeStore()->state().subscribe(
        [this](const source::ThemeMode&) { emit themeModeChanged(); }, false);
    crashSub_ = model_->crashStore()->state().subscribe(
        [this](bool) { emit crashReportingChanged(); }, false);
    backgroundSub_ = model_->backgroundStore()->state().subscribe(
        [this](const source::BackgroundPreferences&) { emit runInBackgroundChanged(); }, false);
    QObject::connect(model_->background(), &composer::BackgroundCoordinator::showWindowRequested,
                     this, &AppViewModel::showWindowRequested);
    QObject::connect(model_->background(), &composer::BackgroundCoordinator::quitRequested, this,
                     &AppViewModel::quitRequested);
    QObject::connect(model_->background(),
                     &composer::BackgroundCoordinator::trayAvailabilityChanged, this,
                     [this](bool) { emit trayAvailableChanged(); });
    onboardingSub_ = model_->onboardingStore()->state().subscribe(
        [this](const source::OnboardingState&) {
            const bool needed = !model_->onboardingStore()->welcomeCompleted();
            if (needed != onboardingNeeded_) {
                onboardingNeeded_ = needed;
                emit onboardingNeededChanged();
            }
        },
        false);
    onboardingNeeded_ = !model_->onboardingStore()->welcomeCompleted();

    // The updater publishes ONE slice; every update property reads this cache,
    // so the whole surface can never disagree with itself mid-frame.
    updateSub_ = model_->updates()->status().subscribe(
        [this](const reducer::UpdateStatus& status) {
            const bool prefsMoved = status.checksEnabled != update_.checksEnabled;
            update_ = status;
            emit updateChanged();
            if (prefsMoved) { emit updatePrefsChanged(); }
        },
        true);
    updatePrefsSub_ = model_->updatePreferenceStore()->state().subscribe(
        [this](const source::UpdatePreferences&) { emit updatePrefsChanged(); }, false);
    QObject::connect(model_->updates(), &update::UpdateChecker::notice, this,
                     [this](reducer::UpdateNotice notice, const QString& version) {
                         emit updateNotice(updateNoticeToken(notice), version);
                     });

    // The composer's derived intent, so the pill cannot claim a reach the
    // preferences no longer ask for.
    keepAwakeSub_ = model_->wakeState().subscribe(
        [this](const composer::WakeState& wake) {
            if (const QString token = keepAwakeReachToken(wake.reach); token != keepAwakeReach_) {
                keepAwakeReach_ = token;
                emit stateChanged();
            }
        },
        true);

    keepAwakePrefsSub_ = model_->keepAwakeStore()->state().subscribe(
        [this](const reducer::KeepAwakePreferences&) { emit keepAwakePrefsChanged(); }, false);

    QObject::connect(model_->featureSettings(), &FeatureSettings::changed, this,
                     [this] { emit lightbarChanged(); });

    // WifiConnection::idFor(server) is already the stable id the sheet knows, so
    // the rejected-PIN forward is verbatim.
    QObject::connect(model_, &dish::AppModel::pairingFailed, this,
                     [this](const QString& connectionId, const QString& reason) {
                         emit pairingFailed(connectionId, reason);
                     });

    // The satellite refusing a controller descriptor is the authoritative
    // negative edge for the apply sequencer's Destination step.
    QObject::connect(model_->wifi(), &net::WifiConnectionManager::slotRegistrationFailed, this,
                     [this](const QString& slotId) {
                         if (apply_.phase != reducer::ApplyPhase::Binding ||
                             slotId != applySlotId_) {
                             return;
                         }
                         dispatchApply(reducer::apply_event::BindRejected{/*unreachable=*/false});
                     });

    // Only System follows the OS scheme; an explicit Light/Dark pick ignores it.
    if (auto* hints = QGuiApplication::styleHints()) {
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            if (model_->themeStore()->mode() != source::ThemeMode::System) { return; }
            ui::setActiveAppearance(ui::detectSystemAppearance());
            if (themeAppliedSink_) {
                themeAppliedSink_(ui::activeAppearance() == ui::Appearance::Dark);
            }
            emit themeModeChanged();
        });
    }

    // ── Apply budgets ────────────────────────────────────────────────────────
    applyPathTimer_ = new QTimer(this);
    applyPathTimer_->setSingleShot(true);
    applyPathTimer_->setInterval(kPathBudgetMs);
    QObject::connect(applyPathTimer_, &QTimer::timeout, this,
                     [this] { dispatchApply(reducer::apply_event::PathTimedOut{}); });
    applyBindTimer_ = new QTimer(this);
    applyBindTimer_->setSingleShot(true);
    applyBindTimer_->setInterval(kBindBudgetMs);
    QObject::connect(applyBindTimer_, &QTimer::timeout, this,
                     [this] { dispatchApply(reducer::apply_event::BindTimedOut{}); });
    applyTickTimer_ = new QTimer(this);
    applyTickTimer_->setInterval(kApplyTickMs);
    QObject::connect(applyTickTimer_, &QTimer::timeout, this, &AppViewModel::onApplyTick);

    const auto radio = source::probeBluetoothRadio();
    bluetoothPresent_ = radio.present;
    bluetoothEnabled_ = radio.enabled;

    telemetryTimer_ = new QTimer(this);
    telemetryTimer_->setInterval(1'000);
    QObject::connect(telemetryTimer_, &QTimer::timeout, this, &AppViewModel::onTelemetryTick);
    telemetryTimer_->start();

    onStateChanged();
    onConnectionsChanged();
    onTelemetryTick();
}

void AppViewModel::onStateChanged() {
    const auto& st = model_->state();

    const auto& conns = st.connections;
    int live = 0;
    QString firstLabel;
    for (const auto& c : conns) {
        if (c.live == models::LinkState::Connected) {
            ++live;
            if (firstLabel.isEmpty()) { firstLabel = c.label; }
        }
    }
    const int total = static_cast<int>(conns.size());
    onlineCount_ = live;
    connectionCount_ = total;

    if (live == 0 && total == 0) {
        statusText_ = tr("No connections yet");
    } else if (live == 0) {
        statusText_ = tr("%n paired", "", total);
    } else if (live == 1) {
        statusText_ = firstLabel;
    } else {
        statusText_ = tr("%n online", "", live);
    }

    if (live == 0 && total == 0) {
        summaryText_ = tr("Open Connections to add one");
    } else if (live == 0) {
        summaryText_ = tr("%n paired", "", total);
    } else {
        // The plural rides the TOTAL, matching Android's status_connected_of:
        // "1 of 1 online" reads singular, "2 of 5 online" plural.
        summaryText_ = tr("%1 of %n online", "", total).arg(live);
    }

    busy_ = st.busy;

    slotCount_ = static_cast<int>(st.slotList.size());
    int bound = 0;
    for (const auto& s : st.slotList) {
        if (s.boundConnectionId.has_value()) { ++bound; }
    }
    boundSlotCount_ = bound;
    firstOnlineName_ = firstLabel;

    // The same pure composer::streamingSlotCount the wake controller keys the
    // display on, fed from this slice's bindings against its links.
    {
        QHash<QString, QString> bindings;
        for (const auto& s : st.slotList) {
            if (s.boundConnectionId.has_value()) { bindings.insert(s.id, *s.boundConnectionId); }
        }
        QHash<QString, models::LinkState> links;
        for (const auto& c : conns) { links.insert(c.id, c.live); }
        streamingSlotCount_ = composer::streamingSlotCount(bindings, links);
    }

    pairingActive_ = st.pairingTarget.has_value();
    pairingServerName_ = pairingActive_ ? st.pairingTarget->name : QString();
    pairingServerId_ = pairingActive_ ? st.pairingTarget->id() : QString();

    slotModel_.setState(st.slotList);

    // Best-effort: the sheet reads a rising online count as "a pair just landed".
    if (live > lastOnlineCount_) { emit pairingSucceeded(); }
    lastOnlineCount_ = live;

    // A device attach or detach also moves the slot list, so the deadzone rows
    // may have changed with it.
    emit deadzonesChanged();

    emit stateChanged();
}

void AppViewModel::onConnectionsChanged() {
    const auto rows = model_->connections()->connections().value();
    connectionModel_.setRows(rows);
    // The slot model's bound-satellite join reads the same derived rows.
    slotModel_.setConnectionRows(rows);

    // Keyed on the id SET rather than the rows, so the per-second latency ticks
    // that also arrive here never churn the FOUND Repeater.
    QSet<QString> ids;
    ids.reserve(static_cast<qsizetype>(rows.size()));
    for (const auto& row : rows) { ids.insert(QString::fromStdString(row.id)); }
    if (ids != connectionRowIds_) {
        connectionRowIds_ = std::move(ids);
        emit discoveredChanged();
    }
}

void AppViewModel::onTelemetryTick() {
    const auto snap = model_->processor()->drainTelemetry();
    eventsPerSec_ = snap.events;
    sendsPerSec_ = snap.sends;
    totalSent_ = snap.totalSent;
    emit telemetryChanged();
}

// Derived on read so no second cache can drift from discoveredServers().
int AppViewModel::foundCount() const { return static_cast<int>(discoveredServers().size()); }

bool AppViewModel::railCollapsed() const { return uiPrefs_.railCollapsed(); }

void AppViewModel::setRailCollapsed(bool collapsed) {
    if (uiPrefs_.railCollapsed() == collapsed) { return; }
    uiPrefs_.setRailCollapsed(collapsed);
    emit railCollapsedChanged();
}

bool AppViewModel::lightbarFollowGame() const {
    return model_->featureSettings()->lightbarMode() == LightbarMode::FollowGame;
}

void AppViewModel::setLightbarFollowGame(bool followGame) {
    model_->featureSettings()->setLightbarMode(followGame ? LightbarMode::FollowGame
                                                          : LightbarMode::Off);
    // FeatureSettings::changed fires too; this keeps the property NOTIFY correct
    // if that connection is ever removed.
    emit lightbarChanged();
}

void AppViewModel::bindSlot(const QString& slotId, const QString& connectionId) {
    model_->hub()->bind(slotId, connectionId);
}

void AppViewModel::unbindSlot(const QString& slotId) {
    // One verb for both destination kinds: a slot rides a satellite OR a
    // Moonlight host, and every caller (the board, the pad card, Configure
    // binding) means the same thing by Unbind.
    if (!model_->moonlightBoundHostFor(slotId).isEmpty()) {
        model_->unbindMoonlightSlot(slotId);
        return;
    }
    model_->hub()->unbind(slotId);
}

namespace {
// A synthetic slot's id IS the packed vpKey string, so it parses. An SDL slot's
// id ("sdl:<iid>") does not, and falls back to the bridge device list. nullopt
// means neither yields an identity.
std::optional<std::pair<int, int>> resolveSlotVidPid(dish::AppModel* model, const QString& slotId) {
    if (const auto parsed = reducer::parseSyntheticSlotId(slotId.toStdString())) { return parsed; }
    for (const auto& d : model->bridge()->devices()) {
        if (d.id == slotId) {
            if (d.vendorId == 0 || d.productId == 0) { return std::nullopt; }
            return std::make_pair(d.vendorId, d.productId);
        }
    }
    return std::nullopt;
}
} // namespace

void AppViewModel::setSlotPath(const QString& slotId, const QString& choice) {
    auto* usb = model_->usbManager();
    if (usb == nullptr) { return; }
    const auto vidPid = resolveSlotVidPid(model_, slotId);
    if (!vidPid.has_value()) { return; }
    const auto [vendorId, productId] = *vidPid;
    // "auto" clears the override and lets the resolution policy decide.
    if (choice == QLatin1String("auto")) {
        usb->clearChoice(vendorId, productId);
    } else if (choice == QLatin1String("direct")) {
        usb->setPathChoice(vendorId, productId, reducer::PathChoice::Direct);
    } else if (choice == QLatin1String("standard")) {
        usb->setPathChoice(vendorId, productId, reducer::PathChoice::Standard);
    }
    // An unrecognised string is ignored. A real pick triggers AppModel's rebuild,
    // which re-emits stateChanged, so the slot roles need no NOTIFY of their own.
}

QVariantList AppViewModel::carriedPads(const QString& connectionId) const {
    QVariantList out;
    for (const auto& pad : reducer::carriedPads(model_->state().slotList, connectionId)) {
        QVariantMap row;
        row[QStringLiteral("name")] = pad.name;
        row[QStringLiteral("emulateName")] = pad.emulateName;
        out.append(row);
    }
    return out;
}

int AppViewModel::slotOrdinal(const QString& slotId, const QString& connectionId) const {
    return reducer::slotOrdinalOnConnection(model_->state().slotList, slotId, connectionId);
}

QVariantList AppViewModel::availableConnectionsForSlot(const QString& slotId) const {
    const auto& st = model_->state();

    // The holdover the picker keeps even while that connection is offline.
    std::optional<QString> boundConnectionId;
    for (const auto& s : st.slotList) {
        if (s.id == slotId) {
            boundConnectionId = s.boundConnectionId;
            break;
        }
    }

    // Candidates are the connections not bound to ANOTHER slot. This slot's own
    // binding stays in, because the reducer's holdover has to see it.
    QList<models::ConnectionSummary> candidates;
    for (const auto& c : st.connections) {
        const bool boundElsewhere = c.boundSlotId.has_value() && *c.boundSlotId != slotId;
        if (!boundElsewhere) { candidates.append(c); }
    }

    QVariantList out;
    for (const auto& c : reducer::connectionsVisibleInPicker(candidates, boundConnectionId)) {
        QVariantMap m;
        m[QStringLiteral("connectionId")] = c.id;
        m[QStringLiteral("label")] = c.label;
        m[QStringLiteral("dotColor")] = dotColorToken(c.live);
        m[QStringLiteral("glyph")] = glyphToken(c.live);
        out.append(m);
    }
    return out;
}

void AppViewModel::refreshEmulate(const QString& slotId) { model_->refreshCatalogForSlot(slotId); }

bool AppViewModel::emulateLoading() const {
    // Loading means "loading AND nothing to show": once types are cached, a
    // background revalidate shows them stale rather than a blank spinner.
    const auto& s = model_->catalogState();
    return s.isLoading() && !s.hasData();
}

QString AppViewModel::emulateError() const {
    // The core carries a reason code, never a string, so the localization
    // happens here at the UI edge. "" means there is no error to show.
    const auto& s = model_->catalogState();
    if (!s.isError() || !s.error.has_value()) { return {}; }
    switch (*s.error) {
    case source::CatalogError::Unreachable:
        return tr("Couldn’t reach the satellite to load controller types.");
    case source::CatalogError::ServerError:
        return tr("The satellite couldn’t provide controller types right now.");
    case source::CatalogError::Malformed:
        return tr("The satellite sent an unreadable controller-type list.");
    }
    return {};
}

bool AppViewModel::emulateStale() const { return model_->catalogState().stale; }

namespace {
QVariantList typeRows(const QList<composer::PickableType>& types) {
    QVariantList out;
    for (const auto& t : types) {
        QVariantMap m;
        m[QStringLiteral("type")] = t.type;
        m[QStringLiteral("slug")] = t.slug;
        m[QStringLiteral("name")] = t.name;
        m[QStringLiteral("shortName")] = t.shortName;
        m[QStringLiteral("description")] = t.description;
        m[QStringLiteral("known")] = t.known;
        out.append(m);
    }
    return out;
}
} // namespace

QVariantList AppViewModel::emulateTypes(const QString& slotId) const {
    return typeRows(model_->pickableTypesFor(slotId));
}

int AppViewModel::emulateCurrentType(const QString& slotId) const {
    return model_->currentTypeFor(slotId);
}

void AppViewModel::refreshEmulateForHost(const QString& connectionId) {
    model_->refreshCatalogForConnection(connectionId);
}

QVariantList AppViewModel::emulateTypesForHost(const QString& connectionId) const {
    return typeRows(model_->pickableTypesForConnection(connectionId));
}

int AppViewModel::emulateCurrentTypeForHost(const QString& connectionId,
                                            const QString& slotId) const {
    return model_->currentTypeForConnection(connectionId, slotId);
}

void AppViewModel::setControllerType(const QString& slotId, int type) {
    model_->setSlotControllerType(slotId, type);
}

void AppViewModel::startDiscovery() { model_->wifi()->startDiscovery(); }

bool AppViewModel::isScanning() const { return model_->wifi()->isScanning(); }

namespace {

QString moonlightLinkToken(source::moon::MoonlightLinkState link) {
    switch (link) {
    case source::moon::MoonlightLinkState::Linking:
        return QStringLiteral("linking");
    case source::moon::MoonlightLinkState::Live:
        return QStringLiteral("live");
    case source::moon::MoonlightLinkState::Failed:
        return QStringLiteral("failed");
    case source::moon::MoonlightLinkState::Idle:
    default:
        return QStringLiteral("idle");
    }
}

} // namespace

QVariantList AppViewModel::moonlightHosts() const {
    QVariantList out;
    for (const auto& row : model_->moonlight()->rows()) {
        QVariantMap m;
        m[QStringLiteral("uuid")] = row.uuid;
        m[QStringLiteral("name")] = row.name;
        m[QStringLiteral("address")] = row.address;
        m[QStringLiteral("paired")] = row.paired;
        m[QStringLiteral("discovered")] = row.discovered;
        m[QStringLiteral("link")] = moonlightLinkToken(row.link);
        m[QStringLiteral("trust")] = QString::fromLatin1(moonlight::hostTrustToken(row.trust));
        m[QStringLiteral("phase")] = QString::fromLatin1(moonlight::hostPhaseToken(row.phase));
        m[QStringLiteral("controllers")] = row.controllers;
        m[QStringLiteral("appId")] = row.lastAppId;
        m[QStringLiteral("appName")] = row.lastAppName;
        m[QStringLiteral("lastAppName")] = row.lastAppName;
        m[QStringLiteral("controllerType")] = row.controllerType;
        out.append(m);
    }
    return out;
}

bool AppViewModel::moonlightScanning() const { return model_->moonlight()->isScanning(); }

bool AppViewModel::moonlightPairingActive() const { return model_->moonlight()->pairingActive(); }

QString AppViewModel::moonlightPairingPin() const { return model_->moonlight()->pairingPin(); }

QString AppViewModel::moonlightPairingHost() const {
    return model_->moonlight()->pairingHostUuid();
}

int AppViewModel::moonlightAutoType() const { return repository::kMoonlightControllerTypeAuto; }

void AppViewModel::scanMoonlight() { model_->moonlight()->startDiscovery(); }

void AppViewModel::addMoonlightHost(const QString& address, const QString& name) {
    if (!address.trimmed().isEmpty()) {
        model_->moonlight()->addManualHost(address.trimmed(), name.trimmed());
    }
}

void AppViewModel::pairMoonlight(const QString& uuid) { model_->moonlight()->pair(uuid); }

void AppViewModel::cancelMoonlightPairing() { model_->moonlight()->cancelPairing(); }

void AppViewModel::forgetMoonlight(const QString& uuid) { model_->moonlight()->forget(uuid); }

void AppViewModel::probeMoonlightHost(const QString& uuid) { model_->moonlight()->probe(uuid); }

void AppViewModel::refreshMoonlightApps(const QString& uuid) {
    model_->moonlight()->refreshApps(uuid);
}

QVariantList AppViewModel::moonlightApps(const QString& uuid) const {
    QVariantList out;
    for (const auto& app : model_->moonlight()->apps(uuid)) {
        QVariantMap m;
        m[QStringLiteral("id")] = app.id;
        m[QStringLiteral("title")] = app.title;
        out.append(m);
    }
    return out;
}

void AppViewModel::setMoonlightApp(const QString& uuid, const QString& appId,
                                   const QString& appName) {
    model_->moonlight()->setLastApp(uuid, appId, appName);
}

void AppViewModel::quitMoonlightApp(const QString& uuid) { model_->moonlight()->quitHostApp(uuid); }

bool AppViewModel::isMoonlightHost(const QString& hostId) const {
    return model_->moonlight()->knows(hostId);
}

QVariantMap AppViewModel::moonlightSession(const QString& uuid, const QString& slotId) const {
    auto* manager = model_->moonlight();
    const auto inputs = manager->uiInputs(uuid, slotId);
    const auto state = moonlight::sessionUiState(inputs);

    QVariantMap m;
    m[QStringLiteral("state")] = QString::fromLatin1(moonlight::sessionUiToken(state));
    m[QStringLiteral("blocksApply")] = moonlight::sessionUiBlocksApply(state);
    m[QStringLiteral("trust")] =
        QString::fromLatin1(moonlight::hostTrustToken(moonlight::hostTrust(inputs)));
    m[QStringLiteral("controllers")] = manager->controllerCount(uuid);
    m[QStringLiteral("maxControllers")] = static_cast<int>(moonlight::kMaxPads);

    QString hostName;
    QString appId;
    QString appName;
    if (const auto row = manager->row(uuid)) {
        hostName = row->name;
        appId = row->lastAppId;
        appName = row->lastAppName;
    }
    // The RUNNING app wins over the remembered pick: a binding that joins a
    // session must name what is actually up, never what we would have started.
    QString refusal;
    if (const auto* session = manager->session(uuid)) {
        if (!session->appId().isEmpty()) {
            appId = session->appId();
            appName = session->appName();
        }
        refusal = session->refusalMessage();
    }
    m[QStringLiteral("refusal")] = refusal;
    m[QStringLiteral("hostName")] = hostName;
    m[QStringLiteral("appId")] = appId;
    m[QStringLiteral("appName")] = appName;

    // 1-based, the way the copy counts: "controller 2 of 4". Zero means this
    // binding holds no number yet.
    int ordinal = 0;
    if (const auto number = manager->controllerNumber(slotId)) {
        ordinal = static_cast<int>(*number) + 1;
    } else if (!slotId.isEmpty()) {
        ordinal = inputs.otherControllers + 1;
    }
    m[QStringLiteral("controllerNumber")] = ordinal;
    return m;
}

int AppViewModel::moonlightResolvedType(const QString& slotId, int candidateType) const {
    bool hasMotion = false;
    if (const auto* slot = slotById(slotId)) { hasMotion = slot->capabilities.hasMotion; }
    return moonlight::resolveControllerType(candidateType, hasMotion);
}

QString AppViewModel::moonlightBoundHost(const QString& slotId) const {
    return model_->moonlightBoundHostFor(slotId);
}

void AppViewModel::setMoonlightControllerType(const QString& uuid, int type) {
    model_->moonlight()->setControllerType(uuid, type);
}

void AppViewModel::bindMoonlight(const QString& slotId, const QString& uuid) {
    model_->bindMoonlightSlot(slotId, uuid);
}

void AppViewModel::unbindMoonlight(const QString& slotId) { model_->unbindMoonlightSlot(slotId); }

QVariantList AppViewModel::discoveredServers() const {
    // The one-spot rule: a satellite that already has a connections row renders
    // there, so FOUND offers only the un-remembered rest. Both sides key on the
    // stable machineId-preferring id, so a remembered box that moved to a fresh
    // DHCP address still folds into its existing row.
    QSet<QString> rowIds;
    for (const auto& row : model_->connections()->connections().value()) {
        rowIds.insert(QString::fromStdString(row.id));
    }
    QVariantList out;
    for (const auto& s :
         reducer::serversVisibleInFound(model_->wifi()->discoveredServers(), rowIds)) {
        QVariantMap m;
        m[QStringLiteral("name")] = s.name;
        m[QStringLiteral("ip")] = s.ip;
        m[QStringLiteral("udpPort")] = s.udpPort;
        m[QStringLiteral("pairPort")] = s.pairPort;
        m[QStringLiteral("httpPort")] = s.httpPort;
        m[QStringLiteral("machineId")] = s.machineId;
        m[QStringLiteral("source")] = models::discoverySourceLabel(s.source);
        m[QStringLiteral("id")] = s.id();
        out.append(m);
    }
    return out;
}

void AppViewModel::connectByServerId(const QString& serverId) {
    for (const auto& s : model_->wifi()->discoveredServers()) {
        if (s.id() == serverId) {
            model_->wifi()->connectTo(s);
            return;
        }
    }
}

void AppViewModel::reconnectConnection(const QString& connectionId) {
    model_->connections()->reconnectConnection(connectionId);
}

void AppViewModel::disconnectConnection(const QString& connectionId) {
    model_->connections()->disconnectConnection(connectionId);
}

void AppViewModel::forgetConnection(const QString& connectionId) {
    model_->connections()->forgetConnection(connectionId);
}

void AppViewModel::pairByServerId(const QString& serverId, const QString& pin) {
    for (const auto& s : model_->wifi()->discoveredServers()) {
        if (s.id() == serverId) {
            model_->wifi()->pairWithPin(s, pin);
            return;
        }
    }
}

bool AppViewModel::isPairingInFlight(const QString& serverId) const {
    return model_->wifi()->isPairingInFlight(serverId);
}

void AppViewModel::clearPairingTarget() { model_->clearPairingTarget(); }

// ── Reverse (host-initiated) pairing ─────────────────────────────────────────

namespace {
// Kept out of the manager so the token the QML sheet switches on stays a
// view-model concern.
QString reversePhaseString(net::ReversePairingPhase phase) {
    switch (phase) {
    case net::ReversePairingPhase::Idle:
        return QStringLiteral("idle");
    case net::ReversePairingPhase::AwaitingApproval:
        return QStringLiteral("awaiting");
    case net::ReversePairingPhase::Approved:
        return QStringLiteral("approved");
    case net::ReversePairingPhase::Declined:
        return QStringLiteral("declined");
    case net::ReversePairingPhase::TimedOut:
        return QStringLiteral("timedout");
    }
    return QStringLiteral("idle");
}
} // namespace

QString AppViewModel::reversePairingPhase() const {
    return reversePhaseString(model_->wifi()->reversePairingPhase());
}

QString AppViewModel::reversePairingPin() const { return model_->wifi()->reversePairingPin(); }

QString AppViewModel::reversePairingServerName() const {
    return model_->wifi()->reversePairingServerName();
}

void AppViewModel::requestReversePairing(const QString& serverId) {
    for (const auto& s : model_->wifi()->discoveredServers()) {
        if (s.id() == serverId) {
            model_->wifi()->requestReversePairing(s);
            return;
        }
    }
}

void AppViewModel::cancelReversePairing() { model_->wifi()->cancelReversePairing(); }

// ── Settings: appearance + diagnostics ──────────────────────────────────────

int AppViewModel::themeMode() const { return themeModeToInt(model_->themeStore()->mode()); }

void AppViewModel::setThemeMode(int mode) {
    const source::ThemeMode next = themeModeFromInt(mode);
    // The ThemeController subscribes to this store and swaps the active palette,
    // so the palette is already current by the time the sink below runs. setMode
    // is distinct-until-changed and themeSub_ re-emits, so no emit here.
    model_->themeStore()->setMode(next);
    if (themeAppliedSink_) { themeAppliedSink_(ui::activeAppearance() == ui::Appearance::Dark); }
}

bool AppViewModel::crashReportingEnabled() const { return model_->crashStore()->enabled(); }

bool AppViewModel::hasCrashReport() const { return crash::hasCrashLog(); }

QString AppViewModel::crashReportText() const {
    return crash::buildReport(crash::readCrashLog(), QString::fromLocal8Bit(qgetenv("HOME")));
}

void AppViewModel::copyCrashReport() const {
    if (auto* clipboard = QGuiApplication::clipboard()) { clipboard->setText(crashReportText()); }
}

void AppViewModel::openCrashIssue() const {
    // openUrl, not a network call: Dish never speaks to GitHub on this path, and
    // the user submits the issue themselves from a page they can still edit.
    QDesktopServices::openUrl(crash::issueUrl(crashReportText()));
}

void AppViewModel::discardCrashReport() {
    if (crash::discardCrashLog()) { emit crashReportChanged(); }
}

void AppViewModel::setCrashReportingEnabled(bool enabled) {
    model_->crashStore()->setEnabled(enabled);
}

bool AppViewModel::runInBackground() const { return model_->backgroundStore()->runInBackground(); }

void AppViewModel::setRunInBackground(bool enabled) {
    model_->backgroundStore()->setRunInBackground(enabled);
}

int AppViewModel::keepAwakeMode() const {
    return keepAwakeModeToInt(model_->keepAwakeStore()->mode());
}

void AppViewModel::setKeepAwakeMode(int mode) {
    model_->keepAwakeStore()->setMode(keepAwakeModeFromInt(mode));
}

int AppViewModel::keepAwakeTimeoutMinutes() const {
    return model_->keepAwakeStore()->idleTimeoutMinutes();
}

void AppViewModel::setKeepAwakeTimeoutMinutes(int minutes) {
    model_->keepAwakeStore()->setIdleTimeoutMinutes(minutes);
}

bool AppViewModel::keepDisplayAwake() const { return model_->keepAwakeStore()->keepDisplayAwake(); }

void AppViewModel::setKeepDisplayAwake(bool enabled) {
    model_->keepAwakeStore()->setKeepDisplayAwake(enabled);
}

bool AppViewModel::trayAvailable() const { return model_->background()->trayAvailable(); }

bool AppViewModel::requestWindowClose() {
    return model_->background()->closeRequested() == reducer::WindowCloseAction::HideToBackground;
}

void AppViewModel::setWindowVisible(bool visible) {
    model_->background()->setWindowVisible(visible);
}

QString AppViewModel::appVersion() const { return QStringLiteral(DISH_VERSION); }

bool AppViewModel::onboardingNeeded() const {
    return !model_->onboardingStore()->welcomeCompleted();
}

QString AppViewModel::donateSponsorsUrl() const {
    return tr("https://github.com/sponsors/TinkerNorth");
}
QString AppViewModel::donateKofiUrl() const { return tr("https://ko-fi.com/tinkernorth"); }
QString AppViewModel::donateBmacUrl() const { return tr("https://buymeacoffee.com/tinkernorth"); }

// ── Deadzone settings ───────────────────────────────────────────────────────

QVariantList AppViewModel::deadzoneDevices() const {
    return deadzoneDeviceRows(model_->bridge(), model_->deadzoneRepository(),
                              model_->motionEnabledStore());
}

void AppViewModel::setDeadzones(const QString& deviceId, int stickFlat, int triggerFlat) {
    const input::deadzone::Deadzones dz{static_cast<std::int16_t>(stickFlat),
                                        static_cast<std::uint8_t>(triggerFlat)};
    // Persisting AND pushing into the live processor is what lets a slider move
    // re-tune the hot path without a re-attach.
    model_->deadzoneRepository()->setDeadzones(deviceId, dz);
    model_->applyDeadzones(deviceId, dz);
    emit deadzonesChanged();
}

void AppViewModel::setMotionEnabled(const QString& deviceId, bool enabled) {
    model_->motionEnabledStore()->setEnabled(deviceId.toStdString(), enabled);
    emit deadzonesChanged();
}

// ── Configure-controls (raw-joystick remap) ──────────────────────────────────

QVariantMap AppViewModel::slotRemap(const QString& slotId) const {
    QVariantMap out;
    const auto vidPid = resolveSlotVidPid(model_, slotId);
    if (!vidPid.has_value()) { return out; }
    const auto [vendorId, productId] = *vidPid;
    const input::JoystickRemap r = model_->remapFor(vendorId, productId);

    // Button values are raw source-button indices, -1 meaning unassigned.
    using Btn = input::RemapButton;
    const auto btn = [&](Btn b) { return r.buttons[static_cast<std::size_t>(b)]; };
    out[QStringLiteral("a")] = btn(Btn::A);
    out[QStringLiteral("b")] = btn(Btn::B);
    out[QStringLiteral("x")] = btn(Btn::X);
    out[QStringLiteral("y")] = btn(Btn::Y);
    out[QStringLiteral("dpadUp")] = btn(Btn::DpadUp);
    out[QStringLiteral("dpadDown")] = btn(Btn::DpadDown);
    out[QStringLiteral("dpadLeft")] = btn(Btn::DpadLeft);
    out[QStringLiteral("dpadRight")] = btn(Btn::DpadRight);
    out[QStringLiteral("leftShoulder")] = btn(Btn::LeftShoulder);
    out[QStringLiteral("rightShoulder")] = btn(Btn::RightShoulder);
    out[QStringLiteral("back")] = btn(Btn::Back);
    out[QStringLiteral("start")] = btn(Btn::Start);
    out[QStringLiteral("leftThumb")] = btn(Btn::LeftThumb);
    out[QStringLiteral("rightThumb")] = btn(Btn::RightThumb);
    out[QStringLiteral("leftStickX")] = r.leftStickX;
    out[QStringLiteral("leftStickY")] = r.leftStickY;
    out[QStringLiteral("rightStickX")] = r.rightStickX;
    out[QStringLiteral("rightStickY")] = r.rightStickY;
    out[QStringLiteral("leftTrigger")] = triggerSourceToMap(r.leftTrigger);
    out[QStringLiteral("rightTrigger")] = triggerSourceToMap(r.rightTrigger);
    out[QStringLiteral("hatIndex")] = r.hatIndex;
    out[QStringLiteral("invertLeftY")] = r.invertLeftY;
    out[QStringLiteral("invertRightY")] = r.invertRightY;
    return out;
}

void AppViewModel::assignSlotInput(const QString& slotId, const QString& target, int kind,
                                   int index) {
    const auto vidPid = resolveSlotVidPid(model_, slotId);
    if (!vidPid.has_value()) { return; }
    const auto tgt = remapTargetFromString(target);
    if (!tgt.has_value()) { return; } // unknown target, forward-compat no-op
    const auto [vendorId, productId] = *vidPid;
    // Persisting pushes into the bridge, so it takes effect on the next report.
    const input::JoystickRemap next =
        input::withAssignment(model_->remapFor(vendorId, productId), *tgt, kind, index);
    model_->setJoystickRemap(vendorId, productId, next);
}

void AppViewModel::setSlotInvert(const QString& slotId, const QString& which, bool on) {
    const auto vidPid = resolveSlotVidPid(model_, slotId);
    if (!vidPid.has_value()) { return; }
    std::optional<input::InvertTarget> inv;
    if (which == QLatin1String("leftY")) {
        inv = input::InvertTarget::LeftY;
    } else if (which == QLatin1String("rightY")) {
        inv = input::InvertTarget::RightY;
    }
    if (!inv.has_value()) { return; } // unknown flag, no-op
    const auto [vendorId, productId] = *vidPid;
    const input::JoystickRemap next =
        input::withInvert(model_->remapFor(vendorId, productId), *inv, on);
    model_->setJoystickRemap(vendorId, productId, next);
}

void AppViewModel::resetSlotRemap(const QString& slotId) {
    const auto vidPid = resolveSlotVidPid(model_, slotId);
    if (!vidPid.has_value()) { return; }
    const auto [vendorId, productId] = *vidPid;
    model_->clearJoystickRemap(vendorId, productId);
}

void AppViewModel::startInputCapture(const QString& slotId) {
    // Capture is global in the bridge; the per-slot filter lives here, so a
    // second start for another slot just re-points it.
    capturingSlotId_ = slotId;
    model_->setInputCaptureEnabled(true);
}

void AppViewModel::stopInputCapture() {
    capturingSlotId_.clear();
    model_->setInputCaptureEnabled(false);
}

void AppViewModel::onRawJoystickInput(const QString& deviceId, int kind, int index, int value) {
    // An SDL slot's id IS the device id, and a synthetic USB-direct slot is never
    // a raw joystick, so matching the deviceId is the whole mapping.
    if (capturingSlotId_.isEmpty() || deviceId != capturingSlotId_) { return; }
    emit rawInputCaptured(capturingSlotId_, kind, index, value);
}

// ── Bluetooth radio ─────────────────────────────────────────────────────────

void AppViewModel::refreshBluetoothState() {
    const auto radio = source::probeBluetoothRadio();
    if (radio.present == bluetoothPresent_ && radio.enabled == bluetoothEnabled_) { return; }
    bluetoothPresent_ = radio.present;
    bluetoothEnabled_ = radio.enabled;
    emit bluetoothChanged();
}

void AppViewModel::openBluetoothSettings() {
    // Routed through the same sink as every other external open, so a failure
    // raises the standard toast.
    openExternalUrl(QStringLiteral("ms-settings:bluetooth"));
}

// ── Capabilities ────────────────────────────────────────────────────────────

const models::ControllerSlot* AppViewModel::slotById(const QString& slotId) const {
    for (const auto& s : model_->state().slotList) {
        if (s.id == slotId) { return &s; }
    }
    return nullptr;
}

bool AppViewModel::catalogResolvedFor(const QString& hostId) const {
    return model_->hasCatalogFor(hostId);
}

QVariantList AppViewModel::capabilityForCandidate(const QString& slotId, int type,
                                                  const QString& hostKind, const QString& hostId,
                                                  const QString& desiredPath, bool motionOn,
                                                  bool rumbleOn, int touchpadMode) const {
    reducer::CapabilityInputs in;

    // An unknown slot leaves the input layer at its defaults rather than
    // inventing capabilities.
    if (const auto* slot = slotById(slotId)) {
        in.padMotion = slot->capabilities.hasMotion;
        in.padTouchpad = slot->capabilities.hasTouchpad;
        in.padLightbar = slot->capabilities.hasLightbar;
        in.padRumble = slot->capabilities.hasRumble;
        // A Bluetooth pad has no USB path to claim, so Direct is unreachable
        // rather than merely unselected.
        in.linkUsb = !slot->bluetooth;
        in.padClaimable = slot->pathSupported;
        in.linkDirect = in.linkUsb && slot->pathSupported && desiredPath == QLatin1String("direct");
    }

    const bool hostIsBluetooth = hostKind == QLatin1String("bluetooth");
    const bool hostIsMoonlight = hostKind == QLatin1String("moonlight");
    in.hostIsBluetooth = hostIsBluetooth;

    if (hostIsMoonlight) {
        // No Moonlight host reports what its emulated devices carry, so there
        // is nothing to wait on and nothing to read: the type layer is the
        // hard-coded table, and the host layer always carries. Crossing out a
        // row we cannot verify would mark every Moonlight binding degraded.
        in.hostResolved = !hostId.isEmpty();
        in.hostMouseControl = true;
        in.hostRumble = true;
        const std::uint8_t resolved = moonlight::resolveControllerType(type, in.padMotion);
        const std::uint8_t ceiling = moonlight::typeCapabilityCeiling(resolved);
        in.typeResolved = true;
        in.typeMotion = (ceiling & moonproto::kCapGyro) != 0;
        in.typeTouchpad = (ceiling & moonproto::kCapTouchpad) != 0;
        in.typeRumble = (ceiling & moonproto::kCapRumble) != 0;
        in.typeLightbar = (ceiling & moonproto::kCapRgbLed) != 0;
        in.userMotionOn = motionOn;
        in.userRumbleOn = rumbleOn;
        in.userTouchpadMode = touchpadMode;

        QVariantList moonRows;
        for (const auto& row : reducer::solveCapabilities(in)) {
            QVariantMap m;
            m[QStringLiteral("feature")] = capFeatureToken(row.feature);
            m[QStringLiteral("inOk")] = row.inOk;
            m[QStringLiteral("linkOk")] = row.linkOk;
            m[QStringLiteral("typeOk")] = row.typeOk;
            m[QStringLiteral("hostOk")] = row.hostOk;
            m[QStringLiteral("verdict")] = capVerdictToken(row.verdict);
            m[QStringLiteral("failingLayer")] = capLayerToken(row.failingLayer);
            m[QStringLiteral("hasFailingLayer")] = row.hasFailingLayer;
            moonRows.append(m);
        }
        return moonRows;
    }

    // A Bluetooth destination is the system gamepad layer, with no catalog to
    // wait on. A satellite is resolved once its catalog lands.
    in.hostResolved = hostIsBluetooth ? !hostId.isEmpty() : model_->hasCatalogFor(hostId);

    if (!hostIsBluetooth && in.hostResolved) {
        const auto hostFeatures = model_->catalogHostFeatures(hostId);
        const auto mouse = hostFeatures.constFind(QStringLiteral("mouseControl"));
        in.hostMouseControl = mouse != hostFeatures.constEnd() && mouse->supported;
        const auto rumble = hostFeatures.constFind(catalog::kFeatureRumble);
        // The rumble return path predates the host block, so a satellite that
        // advertises no block at all still carries it.
        in.hostRumble = rumble == hostFeatures.constEnd() ? true : rumble->supported;
    }

    // Unresolved leaves typeResolved false, which refuses nothing.
    if (const auto typeDto = model_->catalogTypeFor(hostId, type)) {
        const auto known = catalog::knownFeatureSlugs();
        in.typeResolved = true;
        in.typeMotion = reducer::isFeatureOffered(*typeDto, catalog::kFeatureMotion, known);
        in.typeTouchpad = reducer::typeOffersTouchpadDs4(*typeDto);
        in.typeRumble = reducer::isFeatureOffered(*typeDto, catalog::kFeatureRumble, known);
        in.typeLightbar = reducer::isFeatureOffered(*typeDto, catalog::kFeatureLightbar, known);
    }

    in.userMotionOn = motionOn;
    in.userRumbleOn = rumbleOn;
    in.userTouchpadMode = touchpadMode;

    QVariantList out;
    for (const auto& row : reducer::solveCapabilities(in)) {
        QVariantMap m;
        m[QStringLiteral("feature")] = capFeatureToken(row.feature);
        m[QStringLiteral("inOk")] = row.inOk;
        m[QStringLiteral("linkOk")] = row.linkOk;
        m[QStringLiteral("typeOk")] = row.typeOk;
        m[QStringLiteral("hostOk")] = row.hostOk;
        m[QStringLiteral("verdict")] = capVerdictToken(row.verdict);
        m[QStringLiteral("failingLayer")] = capLayerToken(row.failingLayer);
        m[QStringLiteral("hasFailingLayer")] = row.hasFailingLayer;
        out.append(m);
    }
    return out;
}

QVariantList AppViewModel::typeFeatureSummary(const QString& hostId, int type) const {
    const auto typeDto = model_->catalogTypeFor(hostId, type);
    // No catalog, no pills: a guessed "supported" is worse than saying nothing.
    if (!typeDto.has_value()) { return {}; }

    const auto known = catalog::knownFeatureSlugs();
    // The solver's fixed render order, so the preview pills and the full table
    // cannot end up using two vocabularies.
    const std::pair<const char*, bool> rows[] = {
        {"gamepad", true},
        {"triggers", true},
        {"motion", reducer::isFeatureOffered(*typeDto, catalog::kFeatureMotion, known)},
        {"touchpad", reducer::typeOffersTouchpadDs4(*typeDto)},
        {"mouse", true},
        {"rumble", reducer::isFeatureOffered(*typeDto, catalog::kFeatureRumble, known)},
        {"lightbar", reducer::isFeatureOffered(*typeDto, catalog::kFeatureLightbar, known)},
    };
    QVariantList out;
    for (const auto& [feature, supported] : rows) {
        QVariantMap m;
        m[QStringLiteral("feature")] = QString::fromLatin1(feature);
        m[QStringLiteral("supported")] = supported;
        out.append(m);
    }
    return out;
}

// ── Host slot accounting ────────────────────────────────────────────────────

int AppViewModel::hostBoundSlotCount(const QString& connectionId) const {
    if (connectionId.isEmpty()) { return 0; }
    int count = 0;
    for (const auto& s : model_->state().slotList) {
        if (s.boundConnectionId.has_value() && *s.boundConnectionId == connectionId) { ++count; }
    }
    return count;
}

int AppViewModel::hostSlotCapacity() const { return kHostSlotCapacity; }

QString AppViewModel::displacedSlotName(const QString& connectionId) const {
    if (hostBoundSlotCount(connectionId) < kHostSlotCapacity) { return {}; }
    // The board fills in slot-list order, so a fifth bind pushes off the first
    // pad bound to this host.
    for (const auto& s : model_->state().slotList) {
        if (s.boundConnectionId.has_value() && *s.boundConnectionId == connectionId) {
            return s.name;
        }
    }
    return {};
}

// ── Binding-draft helpers ───────────────────────────────────────────────────

QString AppViewModel::resolveSlotIdForBind(const QString& slotId) const {
    const auto& st = model_->state();
    const auto vidPid = resolveSlotVidPid(model_, slotId);
    if (!vidPid.has_value()) {
        // With no (vid, pid) to re-resolve through, the id is all there is, and
        // it is valid exactly as long as the slot is still listed.
        return slotById(slotId) != nullptr ? slotId : QString();
    }
    // A Direct claim retires the framework slot for a synthetic twin under a
    // different id, so the synthetic wins when both are present.
    QString framework;
    for (const auto& s : st.slotList) {
        const auto identity = resolveSlotVidPid(model_, s.id);
        if (!identity.has_value() || *identity != *vidPid) { continue; }
        if (s.usbDirect) { return s.id; }
        if (framework.isEmpty()) { framework = s.id; }
    }
    return framework;
}

bool AppViewModel::isVerifiedModel(const QString& slotId) const {
    const auto* slot = slotById(slotId);
    return slot != nullptr && slot->verifiedModel;
}

QString AppViewModel::touchpadModeFor(const QString& connectionId) const {
    const auto pick = model_->touchpadModeStore()->modeFor(connectionId.toStdString());
    // No invented default: an unpicked host reads "off" and the resolve ladder
    // owns any richer behaviour on the wire.
    return pick.has_value() ? QString::fromStdString(*pick) : QStringLiteral("off");
}

void AppViewModel::setTouchpadMode(const QString& connectionId, const QString& mode) {
    if (connectionId.isEmpty()) { return; }
    if (mode != QLatin1String("off") && mode != QLatin1String("pad") &&
        mode != QLatin1String("mouse")) {
        return; // unrecognised mode, forward-compat no-op
    }
    model_->touchpadModeStore()->setMode(connectionId.toStdString(), mode.toStdString());
}

bool AppViewModel::motionEnabledFor(const QString& slotId) const {
    if (slotId.isEmpty()) { return source::MotionEnabledStore::kDefaultEnabled; }
    return model_->motionEnabledStore()->isEnabled(slotId.toStdString());
}

// TODO: rumble rides the descriptor caps because no per-binding rumble store
// exists yet. A RumbleEnabledStore mirroring MotionEnabledStore is what these
// two need to become real.
bool AppViewModel::rumbleEnabledFor(const QString& /*slotId*/) const { return true; }

void AppViewModel::setRumbleEnabled(const QString& /*slotId*/, bool /*on*/) {}

QString AppViewModel::discoverySourceFor(const QString& serverId) const {
    for (const auto& s : model_->wifi()->discoveredServers()) {
        if (s.id() == serverId) { return models::discoverySourceLabel(s.source); }
    }
    return {};
}

// ── Apply ───────────────────────────────────────────────────────────────────

bool AppViewModel::applyInFlight() const { return reducer::applyInFlight(apply_); }
bool AppViewModel::applyCancellable() const { return reducer::applyCancellable(apply_); }
QString AppViewModel::applyConnectionState() const { return applyStepToken(apply_.connection); }
QString AppViewModel::applyDestinationState() const { return applyStepToken(apply_.destination); }

void AppViewModel::applyBinding(const QString& slotId, const QString& connectionId, int type,
                                const QString& desiredPath, bool motionOn, bool rumbleOn,
                                int touchpadMode) {
    if (applyInFlight()) { return; } // one run at a time; the overlay is modal

    // Re-resolve before anything is written: an earlier path switch can have
    // retired the id the page opened with, and binding a dead id silently does
    // nothing.
    const QString resolved = resolveSlotIdForBind(slotId);
    if (resolved.isEmpty()) {
        apply_ = reducer::ApplyState{};
        emit applyChanged();
        emit applyFinished(false, QStringLiteral("slotGone"), false);
        return;
    }

    applySlotId_ = resolved;
    applyConnectionId_ = connectionId;
    applyType_ = type;
    applyIsMoonlight_ = model_->moonlight()->knows(connectionId);
    applyMotionOn_ = motionOn;
    applyRumbleOn_ = rumbleOn;
    applyTouchpadMode_ = touchpadMode;

    // Only a claimable wired pad has a path at all, and only a genuine change is
    // worth a 20 s budget.
    const auto* slot = slotById(resolved);
    const bool wantsDirect = desiredPath == QLatin1String("direct");
    bool needsPathSwitch = false;
    if (slot != nullptr && slot->pathSupported && !slot->bluetooth &&
        (desiredPath == QLatin1String("direct") || desiredPath == QLatin1String("standard"))) {
        const bool currentlyDirect = slot->desiredPath == reducer::PathChoice::Direct;
        needsPathSwitch = currentlyDirect != wantsDirect;
    }

    dispatchApply(reducer::apply_event::Start{needsPathSwitch, wantsDirect});
    applyTickTimer_->start();
    if (needsPathSwitch) {
        applyPathTimer_->start();
        setSlotPath(resolved, desiredPath);
        return; // the tick sees the FSM settle and hands off to the bind
    }
    beginApplyBind();
}

void AppViewModel::beginApplyBind() {
    // Every setting is written BEFORE the binding, because ConnectionHub::bind
    // resolves the type, the motion grant and the touchpad mode while it builds
    // the descriptor. One PUT then carries the whole draft; writing them after
    // would re-attach the slot once per setting. The type goes straight into the
    // store because AppModel::setSlotControllerType needs an existing binding
    // and would bind a second time.
    if (applyType_ > 0 || (applyIsMoonlight_ && applyType_ != -1)) {
        // A Moonlight binding stores its Auto sentinel too: 0xFF is a real pick
        // there, resolved against the pad at CONTROLLER_ARRIVAL time.
        model_->typeStore()->setType(applyConnectionId_.toStdString(), applySlotId_.toStdString(),
                                     applyType_);
    }
    setMotionEnabled(applySlotId_, applyMotionOn_);
    setTouchpadMode(applyConnectionId_, applyTouchpadMode_ == 2   ? QStringLiteral("mouse")
                                        : applyTouchpadMode_ == 1 ? QStringLiteral("pad")
                                                                  : QStringLiteral("off"));
    setRumbleEnabled(applySlotId_, applyRumbleOn_);
    if (applyIsMoonlight_) {
        // The host remembers the last pick so the NEXT binding on it starts
        // where this one did. The binding still owns the type it sends; this is
        // a seed, not the authority.
        setMoonlightControllerType(applyConnectionId_, applyType_);
        // A binding to a Moonlight host is a durable INTENT, not a handshake:
        // there is no descriptor to PUT and nothing to be refused by. The
        // session is started or joined here if the host is already trusted and
        // reachable, and its state is reported by the session section rather
        // than by failing the apply. Nothing about the host may block saving
        // what the user asked for.
        bindMoonlight(applySlotId_, applyConnectionId_);
        dispatchApply(reducer::apply_event::BindAccepted{});
        return;
    }
    bindSlot(applySlotId_, applyConnectionId_);
    applyBindTimer_->start();
}

void AppViewModel::cancelApply() { dispatchApply(reducer::apply_event::Cancel{}); }

void AppViewModel::dispatchApply(const reducer::ApplyEvent& event) {
    const reducer::ApplyState before = apply_;
    apply_ = reducer::reduceApply(before, event);
    if (apply_.phase == before.phase && apply_.connection == before.connection &&
        apply_.destination == before.destination &&
        apply_.elapsedMsOnStep == before.elapsedMsOnStep &&
        apply_.directFellBack == before.directFellBack) {
        return; // the machine refused the event, so nothing moved
    }

    const bool enteredBinding = before.phase == reducer::ApplyPhase::SwitchingPath &&
                                apply_.phase == reducer::ApplyPhase::Binding;
    if (before.phase == reducer::ApplyPhase::SwitchingPath &&
        apply_.phase != reducer::ApplyPhase::SwitchingPath) {
        applyPathTimer_->stop();
    }
    if (!reducer::applyInFlight(apply_)) {
        applyPathTimer_->stop();
        applyBindTimer_->stop();
        applyTickTimer_->stop();
    }

    emit applyChanged();
    if (enteredBinding) { beginApplyBind(); }

    switch (apply_.phase) {
    case reducer::ApplyPhase::Succeeded:
        emit applyFinished(true, QString(), apply_.directFellBack);
        break;
    case reducer::ApplyPhase::Failed:
    case reducer::ApplyPhase::Cancelled:
        emit applyFinished(false,
                           apply_.failure.has_value() ? applyFailureToken(*apply_.failure)
                                                      : QStringLiteral("bindRejected"),
                           apply_.directFellBack);
        break;
    default:
        break;
    }
}

void AppViewModel::onApplyTick() {
    dispatchApply(reducer::apply_event::Tick{kApplyTickMs});
    if (!applyInFlight()) { return; }

    // The pad going away mid-apply is terminal from either step.
    const auto* slot = slotById(applySlotId_);
    if (slot == nullptr && resolveSlotIdForBind(applySlotId_).isEmpty()) {
        dispatchApply(reducer::apply_event::SlotVanished{});
        return;
    }

    if (apply_.phase == reducer::ApplyPhase::SwitchingPath) {
        // The same derived predicate the slot card's spinner reads, so the
        // overlay and the card cannot disagree about when a claim has settled.
        const QString liveId = resolveSlotIdForBind(applySlotId_);
        if (liveId != applySlotId_ && !liveId.isEmpty()) { applySlotId_ = liveId; }
        const auto* live = slotById(applySlotId_);
        if (live == nullptr) { return; }
        const bool switching = reducer::slotPathSwitching(
            live->pathPhase, live->desiredPath, live->usbDirect, live->liveRates.directPollHz,
            live->directFailure.has_value());
        if (!switching) {
            dispatchApply(
                reducer::apply_event::PathSettled{live->pathPhase == reducer::UsbPhase::Direct});
        }
        return;
    }

    // A Moonlight bind settles the instant it is written, so there is no
    // readback to wait on and no host state that could turn it into a failure.
    if (applyIsMoonlight_) {
        // The host remembers the last pick so the NEXT binding on it starts
        // where this one did. The binding still owns the type it sends; this is
        // a seed, not the authority.
        setMoonlightControllerType(applyConnectionId_, applyType_);
        return;
    }

    // The hub binds locally and the satellite answers asynchronously, so the
    // outcome must never be read on the tick that ENTERED this step: the local
    // bind is synchronous and a same-tick read reports success before the
    // satellite has had a chance to refuse.
    if (slot == nullptr || apply_.elapsedMsOnStep <= 0) { return; }
    const bool stillBound =
        slot->boundConnectionId.has_value() && *slot->boundConnectionId == applyConnectionId_;
    if (!stillBound) {
        dispatchApply(reducer::apply_event::BindRejected{/*unreachable=*/false});
        return;
    }
    if (slot->boundStatus.has_value() && slot->boundStatus->live == models::LinkState::Connected) {
        dispatchApply(reducer::apply_event::BindAccepted{});
    }
}

// ── Licenses ────────────────────────────────────────────────────────────────

QVariantList AppViewModel::licenses() const {
    return licenseRows(ui::loadBundledLicenseManifest());
}

// ── Onboarding + external links ─────────────────────────────────────────────

void AppViewModel::markOnboardingComplete() {
    // onboardingSub_ emits onboardingNeededChanged on the store republish.
    model_->onboardingStore()->markWelcomeCompleted();
}

void AppViewModel::openExternalUrl(const QString& url) {
    if (url.isEmpty()) { return; }
    // The sink returns false when it could not hand the URL off, and errorMessage
    // is the QML toast channel. Without a sink, tests take the same path.
    const bool ok =
        externalOpenSink_ ? externalOpenSink_(url) : QDesktopServices::openUrl(QUrl(url));
    if (!ok) { emit errorMessage(tr("Couldn’t open browser")); }
}

// ── Auto-updater ────────────────────────────────────────────────────────────

QString AppViewModel::updatePhase() const { return updatePhaseToken(update_.phase); }

QString AppViewModel::updateVersion() const { return update_.availableVersion; }

QString AppViewModel::updateErrorToken() const {
    if (update_.phase != reducer::UpdatePhase::Failed) { return {}; }
    return updateErrorTokenFor(update_.error);
}

QString AppViewModel::updateNotesUrl() const { return update_.notesUrl; }

QDateTime AppViewModel::updateLastCheck() const { return model_->updates()->lastCheck(); }

void AppViewModel::setUpdateChecksEnabled(bool enabled) {
    model_->updatePreferenceStore()->setChecksEnabled(enabled);
}

void AppViewModel::checkForUpdatesNow() { model_->updates()->checkNow(); }

void AppViewModel::skipUpdate() { model_->updates()->skipAvailableVersion(); }

void AppViewModel::openReleaseNotes() {
    const QString url =
        update_.notesUrl.isEmpty() ? QString::fromLatin1(kReleasesPageUrl) : update_.notesUrl;
    openExternalUrl(url);
}

} // namespace dish::qml
