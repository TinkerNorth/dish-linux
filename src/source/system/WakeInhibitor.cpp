// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/WakeInhibitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QLoggingCategory>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcDishWake, "dish.wake")

QString screenSaverService() { return QStringLiteral("org.freedesktop.ScreenSaver"); }
QString screenSaverPath() { return QStringLiteral("/org/freedesktop/ScreenSaver"); }
QString logindService() { return QStringLiteral("org.freedesktop.login1"); }
QString logindPath() { return QStringLiteral("/org/freedesktop/login1"); }
QString logindManager() { return QStringLiteral("org.freedesktop.login1.Manager"); }

} // namespace

FreedesktopWakeInhibitor::FreedesktopWakeInhibitor(QObject* parent) : WakeInhibitor(parent) {}

FreedesktopWakeInhibitor::~FreedesktopWakeInhibitor() {
    // RAII: never leak a cookie even if the owner forgot to release. A session
    // bus that died on logout is fine too — D-Bus drops cookies with the peer.
    releaseDisplay();
    releaseSystem();
}

// The greatest reach actually satisfied, not the one asked for. A display hold
// with no system hold under it is a degraded logind, already warned about, and
// the enum has no word for it — so it reports None rather than overclaiming.
reducer::KeepAwakeReach FreedesktopWakeInhibitor::held() const {
    if (!logindFd_.isValid()) { return reducer::KeepAwakeReach::None; }
    return cookie_.has_value() ? reducer::KeepAwakeReach::SystemAndDisplay
                               : reducer::KeepAwakeReach::System;
}

void FreedesktopWakeInhibitor::apply(reducer::KeepAwakeReach reach, const QString& reason) {
    const bool wantSystem = reach != reducer::KeepAwakeReach::None;
    const bool wantDisplay = reach == reducer::KeepAwakeReach::SystemAndDisplay;

    if (wantSystem) {
        acquireSystem(reason);
    } else {
        releaseSystem();
    }
    if (wantDisplay) {
        acquireDisplay(reason);
    } else {
        releaseDisplay();
    }
}

// "idle", not "sleep": the analogue of ES_SYSTEM_REQUIRED. "block" on "idle"
// needs no polkit grant for an active session.
void FreedesktopWakeInhibitor::acquireSystem(const QString& reason) {
    if (logindFd_.isValid()) { return; }
    QDBusInterface iface(logindService(), logindPath(), logindManager(),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        qCDebug(lcDishWake) << "logind unavailable; no idle inhibit";
        return;
    }
    const QDBusReply<QDBusUnixFileDescriptor> reply =
        iface.call(QStringLiteral("Inhibit"), QStringLiteral("idle"), QStringLiteral("Dish"),
                   reason, QStringLiteral("block"));
    if (!reply.isValid()) {
        qCWarning(lcDishWake) << "login1.Inhibit failed:" << reply.error().message();
        return;
    }
    logindFd_ = reply.value();
}

void FreedesktopWakeInhibitor::releaseSystem() { logindFd_ = QDBusUnixFileDescriptor(); }

void FreedesktopWakeInhibitor::acquireDisplay(const QString& reason) {
    if (cookie_.has_value()) { return; }
    QDBusInterface iface(screenSaverService(), screenSaverPath(), screenSaverService(),
                         QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        qCWarning(lcDishWake) << "org.freedesktop.ScreenSaver unavailable on session bus:"
                              << iface.lastError().message();
        return;
    }
    const QDBusReply<unsigned int> reply =
        iface.call(QStringLiteral("Inhibit"), QStringLiteral("Dish"), reason);
    if (!reply.isValid()) {
        qCWarning(lcDishWake) << "ScreenSaver.Inhibit failed:" << reply.error().message();
        return;
    }
    cookie_ = reply.value();
    qCDebug(lcDishWake) << "ScreenSaver.Inhibit cookie=" << *cookie_ << "reason=" << reason;
}

void FreedesktopWakeInhibitor::releaseDisplay() {
    if (!cookie_.has_value()) { return; }
    QDBusInterface iface(screenSaverService(), screenSaverPath(), screenSaverService(),
                         QDBusConnection::sessionBus());
    if (iface.isValid()) { iface.call(QStringLiteral("UnInhibit"), *cookie_); }
    qCDebug(lcDishWake) << "ScreenSaver.UnInhibit cookie=" << *cookie_;
    cookie_.reset();
}

} // namespace dish::source
