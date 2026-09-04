// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/CrashReportingBackend.h"

#include <QDir>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <cstdlib>
#include <utility>

// Injected by CMake. The fallback keeps this translation unit compilable on its
// own, which is the state the unit tests build it in: an empty DSN, so a test
// run can never transmit.
#ifndef DISH_SENTRY_DSN
#define DISH_SENTRY_DSN ""
#endif
#ifndef DISH_SENTRY_RELEASE
#define DISH_SENTRY_RELEASE "dish-linux@unknown"
#endif

#ifdef DISH_HAS_SENTRY
#include <sentry.h>
#endif

namespace dish::composer {

namespace {
Q_LOGGING_CATEGORY(lcCrash, "dish.crash")

// $SENTRY_DSN, or empty when unset: the escape hatch for pointing a local build
// at a scratch project.
std::string envDsn() {
    const char* raw = std::getenv("SENTRY_DSN");
    return raw != nullptr ? std::string(raw) : std::string();
}

// AppLocalDataLocation is per-user and writable. Created eagerly because
// sentry_init on a missing directory just fails.
std::string defaultDatabaseDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) { return {}; }
    const QString dir = base + QStringLiteral("/sentry");
    QDir().mkpath(dir);
    return dir.toStdString();
}
} // namespace

bool sentrySdkAvailable() {
#ifdef DISH_HAS_SENTRY
    return true;
#else
    return false;
#endif
}

const char* compiledSentryDsn() { return DISH_SENTRY_DSN; }

const char* sentryEnvironment() {
    return compiledSentryDsn()[0] != '\0' ? "production" : "development";
}

bool shouldArmSentry(const char* compiledDsn, const char* envOverride, bool userEnabled) {
    // The switch is checked first and is never bypassed. $SENTRY_DSN lets a
    // developer aim a build at a scratch project; it is not a way to report
    // from a machine whose owner turned this off.
    if (!userEnabled) { return false; }
    const bool haveCompiled = compiledDsn != nullptr && compiledDsn[0] != '\0';
    const bool haveOverride = envOverride != nullptr && envOverride[0] != '\0';
    return haveCompiled || haveOverride;
}

SentryCrashReportingBackend::SentryCrashReportingBackend(std::string databaseDir)
    : databaseDir_(std::move(databaseDir)) {}

void SentryCrashReportingBackend::disarm() noexcept {
    if (!active_) { return; }
    active_ = false;
#ifdef DISH_HAS_SENTRY
    // Flushes what is already queued, then closes. An opt-out must not strand
    // a report the user has just declined to send.
    sentry_close();
#endif
}

SentryCrashReportingBackend::~SentryCrashReportingBackend() { disarm(); }

void SentryCrashReportingBackend::setEnabled(bool enabled) {
    if (!enabled) {
        const bool wasArmed = active_;
        disarm();
        if (wasArmed) { qCInfo(lcCrash) << "crash reporting disarmed"; }
        return;
    }

    if (active_) { return; }

    const std::string envOverride = envDsn();
    if (!shouldArmSentry(compiledSentryDsn(), envOverride.c_str(), true)) {
        // The common case for anything but a release build, and not a problem:
        // the local crash.log is still written either way.
        qCInfo(lcCrash) << "crash reporting requested but this build carries no DSN;"
                        << "local crash reports are still written";
        return;
    }

#ifdef DISH_HAS_SENTRY
    sentry_options_t* options = sentry_options_new();

    // Leave the DSN unset when only $SENTRY_DSN is present: the SDK reads the
    // environment itself, and an empty string here would override it.
    if (compiledSentryDsn()[0] != '\0') { sentry_options_set_dsn(options, compiledSentryDsn()); }

    const std::string dir = databaseDir_.empty() ? defaultDatabaseDir() : databaseDir_;
    sentry_options_set_database_path(options, dir.c_str());
    sentry_options_set_release(options, DISH_SENTRY_RELEASE);
    sentry_options_set_environment(options, sentryEnvironment());
    sentry_options_set_debug(options, 0);

    // Defaults to on, and would report every launch and quit of a desktop app.
    // The crash is the payload; the rest is telemetry nobody agreed to when
    // they left a switch labelled "crash reports" alone.
    sentry_options_set_auto_session_tracking(options, 0);

    // No sentry_options_set_send_default_pii() call on purpose: in
    // sentry-native that setter exists only under SENTRY_PLATFORM_NX, and its
    // own documentation states that not sending PII is already the default
    // everywhere. Calling it would not compile here.
    //
    // No handler path either: this build selects SENTRY_BACKEND=inproc (see
    // CMakeLists), which runs in-process and needs no helper executable. That
    // is the whole reason for the choice, since a helper would otherwise have
    // to be threaded through the .deb, the .rpm and the AppImage separately.

    if (sentry_init(options) == 0) {
        active_ = true;
        qCInfo(lcCrash) << "crash reporting armed (" << sentryEnvironment() << ")";
    } else {
        qCWarning(lcCrash) << "sentry_init failed; local crash reports are still written";
    }
#endif
}

} // namespace dish::composer
