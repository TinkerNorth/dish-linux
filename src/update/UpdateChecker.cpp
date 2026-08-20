// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "update/UpdateChecker.h"

#include "update/HttpGateways.h"

#include <QNetworkInformation>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <utility>

#ifndef DISH_VERSION
#define DISH_VERSION "0.0.0"
#endif

namespace dish::update {

namespace sched = dish::reducer::update_schedule;

namespace {

qint64 systemClockMs() { return QDateTime::currentMSecsSinceEpoch(); }

} // namespace

UpdateChecker::UpdateChecker(source::UpdatePreferenceStore* prefs, QObject* parent)
    : UpdateChecker(prefs, std::make_unique<HttpManifestGateway>(), parent) {
    // Production only. The gateway-injecting constructor is the test seam, and
    // loading the backend there would start NetworkManager's D-Bus thread in
    // every test that builds a checker.
    hookConnectivity();
}

UpdateChecker::UpdateChecker(source::UpdatePreferenceStore* prefs,
                             std::unique_ptr<ManifestGateway> gateway, QObject* parent)
    : QObject(parent), prefs_(prefs), gateway_(std::move(gateway)), clock_(systemClockMs) {
    construct();
}

UpdateChecker::~UpdateChecker() {
    if (gateway_ != nullptr) { gateway_->cancel(); }
}

void UpdateChecker::construct() {
    reducer::UpdateStatus initial;
    initial.currentVersion = QString::fromLatin1(DISH_VERSION);
    status_.set(initial);

    checkTimer_ = new QTimer(this);
    checkTimer_->setSingleShot(true);
    QObject::connect(checkTimer_, &QTimer::timeout, this, [this] {
        pendingCheckDelayMs_ = -1;
        dispatch(reducer::update_event::CheckRequested{reducer::UpdateTrigger::Periodic});
    });

    if (prefs_ != nullptr) {
        prefsSub_ = prefs_->state().subscribe(
            [this](const source::UpdatePreferences& p) { onPrefsChanged(p); });
    }
}

// Without this the reducer's offline gate never fires: `online` sits at its
// `true` default, a captive portal's HTML splash returns 200 and surfaces as
// ManifestInvalid, and the 30 s recheck after a link returns is replaced by
// the full failure ladder — up to six hours.
void UpdateChecker::hookConnectivity() {
    // A backend that will not load degrades to always-attempt: worst case the
    // request fails and backs off, which is strictly better than never trying.
    if (!QNetworkInformation::loadDefaultBackend()) { return; }
    auto* info = QNetworkInformation::instance();
    if (info == nullptr) { return; }

    const auto readOnline = [](QNetworkInformation* source) {
        if (source->supports(QNetworkInformation::Feature::CaptivePortal) &&
            source->isBehindCaptivePortal()) {
            return false;
        }
        if (!source->supports(QNetworkInformation::Feature::Reachability)) { return true; }
        return source->reachability() == QNetworkInformation::Reachability::Online;
    };

    dispatch(reducer::update_event::ReachabilityChanged{readOnline(info)});

    QObject::connect(info, &QNetworkInformation::reachabilityChanged, this,
                     [this, info, readOnline](QNetworkInformation::Reachability) {
                         dispatch(reducer::update_event::ReachabilityChanged{readOnline(info)});
                     });
    QObject::connect(info, &QNetworkInformation::isBehindCaptivePortalChanged, this,
                     [this, info, readOnline](bool) {
                         dispatch(reducer::update_event::ReachabilityChanged{readOnline(info)});
                     });
}

void UpdateChecker::onPrefsChanged(const source::UpdatePreferences& prefs) {
    dispatch(reducer::update_event::PrefsChanged{prefs.checksEnabled, prefs.skippedVersion});
}

void UpdateChecker::setClock(ClockFn clock) {
    if (clock) { clock_ = std::move(clock); }
}

void UpdateChecker::setCurrentVersion(const QString& version) {
    reducer::UpdateStatus next = status_.value();
    next.currentVersion = version;
    status_.set(next);
}

void UpdateChecker::start() {
    if (started_) { return; }
    started_ = true;
    if (!status_.value().checksEnabled) { return; }

    // The gap is measured from the last COMPLETED check so a relaunch loop
    // cannot hammer the permalink. A stored time in the future is a clock that
    // moved backwards; beyond the escape window it is ignored rather than
    // trusted, which costs one extra check instead of permanent silence.
    const qint64 now = clock_();
    QSettings settings;
    const qint64 last =
        settings.value(QLatin1String(source::kKeyUpdatesLastCheckUtcMs), 0).toLongLong();
    const qint64 elapsed = now - last;
    const bool skewed = elapsed < -sched::kFutureSkewEscapeMs;
    // No `elapsed >= 0` term: it made !skewed unreachable, so the escape window
    // never decided anything and a stored time one millisecond in the future
    // took the same path as one a century out.
    const bool withinGap = last > 0 && !skewed && elapsed < sched::kMinCheckGapMs;

    // Clamped because a future timestamp inside the window gives a remainder
    // longer than the gap itself, which would overflow the cast.
    const qint64 remaining =
        std::clamp<qint64>(sched::kMinCheckGapMs - elapsed, 0, sched::kMinCheckGapMs);
    const int delay = withinGap ? static_cast<int>(remaining) : sched::kStartupDelayMs;
    pendingCheckDelayMs_ = delay;
    checkTimer_->start(delay);
}

void UpdateChecker::checkNow() {
    const qint64 now = clock_();
    if (lastManualCheckMs_ != 0 && now - lastManualCheckMs_ < sched::kManualMinGapMs) { return; }
    lastManualCheckMs_ = now;
    dispatch(reducer::update_event::CheckRequested{reducer::UpdateTrigger::Manual});
}

void UpdateChecker::skipAvailableVersion() {
    // `required` guards the STORE write, not just the dispatch: the reducer
    // refuses a required SkipRequested, but a persisted skippedVersion comes
    // back as PrefsChanged and mutes the unsupported-build nag for good.
    const reducer::UpdateStatus current = status_.value();
    if (current.availableVersion.isEmpty() || current.required) { return; }
    if (prefs_ != nullptr) { prefs_->setSkippedVersion(current.availableVersion); }
    dispatch(reducer::update_event::SkipRequested{current.availableVersion});
}

QDateTime UpdateChecker::lastCheck() const {
    QSettings settings;
    const qint64 ms =
        settings.value(QLatin1String(source::kKeyUpdatesLastCheckUtcMs), 0).toLongLong();
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC) : QDateTime();
}

void UpdateChecker::firePendingCheck() {
    if (pendingCheckDelayMs_ < 0) { return; }
    pendingCheckDelayMs_ = -1;
    checkTimer_->stop();
    dispatch(reducer::update_event::CheckRequested{reducer::UpdateTrigger::Periodic});
}

void UpdateChecker::persistLastCheck() {
    QSettings settings;
    settings.setValue(QLatin1String(source::kKeyUpdatesLastCheckUtcMs), clock_());
    settings.sync();
}

void UpdateChecker::dispatch(const reducer::UpdateEvent& event) {
    const auto reduction = reducer::reduceUpdate(status_.value(), event);
    status_.set(reduction.next);
    for (const auto& effect : reduction.effects) { execute(effect); }
}

void UpdateChecker::execute(const reducer::UpdateEffect& effect) {
    if (std::get_if<reducer::update_effect::FetchManifest>(&effect) != nullptr) {
        if (gateway_ == nullptr) {
            dispatch(reducer::update_event::CheckFailed{reducer::UpdateError::Http});
            return;
        }
        gateway_->fetch([this](const ManifestFetchResult& result) {
            if (!result.manifest.has_value()) {
                dispatch(reducer::update_event::CheckFailed{result.error});
                return;
            }
            dispatch(reducer::update_event::ManifestArrived{*result.manifest});
        });
        return;
    }

    if (const auto* schedule = std::get_if<reducer::update_effect::ScheduleNextCheck>(&effect)) {
        if (!status_.value().checksEnabled) { return; }
        int delay = schedule->delayMs;
        // Jitter the failure ladder only: the 15 s startup delay and the 4 h
        // interval are asserted verbatim, and spreading retries is what the
        // jitter is for.
        if (status_.value().phase == reducer::UpdatePhase::Failed) {
            delay = sched::jitteredDelayMs(delay, QRandomGenerator::global()->generateDouble());
        }
        pendingCheckDelayMs_ = delay;
        checkTimer_->start(delay);
        return;
    }

    if (std::get_if<reducer::update_effect::PersistLastCheck>(&effect) != nullptr) {
        persistLastCheck();
        return;
    }

    if (const auto* n = std::get_if<reducer::update_effect::Notify>(&effect)) {
        emit notice(n->notice, n->version);
    }
}

} // namespace dish::update
