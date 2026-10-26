// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/WakeInhibitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QLoggingCategory>

#include <utility>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcDishWake, "dish.wake")

QString screenSaverService() { return QStringLiteral("org.freedesktop.ScreenSaver"); }
QString screenSaverPath() { return QStringLiteral("/org/freedesktop/ScreenSaver"); }
QString logindService() { return QStringLiteral("org.freedesktop.login1"); }
QString logindPath() { return QStringLiteral("/org/freedesktop/login1"); }
QString logindManager() { return QStringLiteral("org.freedesktop.login1.Manager"); }
QString idleLock() { return QStringLiteral("idle"); }
QString lidLock() { return QStringLiteral("handle-lid-switch"); }

// "block" on "idle" needs no polkit grant for an active session, and upstream
// polkit allows "handle-lid-switch" for active and inactive sessions alike.
QDBusUnixFileDescriptor callLogindInhibit(const QString& what, const QString& reason) {
    QDBusInterface iface(logindService(), logindPath(), logindManager(),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        qCDebug(lcDishWake) << "logind unavailable; no" << what << "inhibit";
        return {};
    }
    const QDBusReply<QDBusUnixFileDescriptor> reply = iface.call(
        QStringLiteral("Inhibit"), what, QStringLiteral("Dish"), reason, QStringLiteral("block"));
    if (!reply.isValid()) {
        qCWarning(lcDishWake) << "login1.Inhibit" << what << "failed:" << reply.error().message();
        return {};
    }
    return reply.value();
}

} // namespace

FreedesktopWakeInhibitor::FreedesktopWakeInhibitor(QObject* parent)
    : FreedesktopWakeInhibitor(callLogindInhibit, parent) {}

FreedesktopWakeInhibitor::FreedesktopWakeInhibitor(LogindInhibit inhibit, QObject* parent)
    : WakeInhibitor(parent), inhibit_(std::move(inhibit)) {}

FreedesktopWakeInhibitor::~FreedesktopWakeInhibitor() {
    // RAII: never leak a cookie even if the owner forgot to release. A session
    // bus that died on logout is fine too — D-Bus drops cookies with the peer.
    releaseLid();
    releaseDisplay();
    releaseSystem();
}

reducer::KeepAwakeHold FreedesktopWakeInhibitor::held() const {
    return reducer::KeepAwakeHold{heldReach(), lidFd_.isValid()};
}

// The greatest reach actually satisfied, not the one asked for. A display hold
// with no system hold under it is a degraded logind, already warned about, and
// the enum has no word for it — so it reports None rather than overclaiming.
reducer::KeepAwakeReach FreedesktopWakeInhibitor::heldReach() const {
    if (!logindFd_.isValid()) { return reducer::KeepAwakeReach::None; }
    return cookie_.has_value() ? reducer::KeepAwakeReach::SystemAndDisplay
                               : reducer::KeepAwakeReach::System;
}

void FreedesktopWakeInhibitor::apply(const reducer::KeepAwakeHold& hold, const QString& reason) {
    const bool wantSystem = hold.reach != reducer::KeepAwakeReach::None;
    const bool wantDisplay = hold.reach == reducer::KeepAwakeReach::SystemAndDisplay;

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
    if (hold.lid) {
        acquireLid(reason);
    } else {
        releaseLid();
    }
}

// "idle", not "sleep": the analogue of ES_SYSTEM_REQUIRED.
void FreedesktopWakeInhibitor::acquireSystem(const QString& reason) {
    if (logindFd_.isValid()) { return; }
    logindFd_ = inhibit_(idleLock(), reason);
}

void FreedesktopWakeInhibitor::releaseSystem() { logindFd_ = QDBusUnixFileDescriptor(); }

// logind.conf(5): low-level locks such as this one are always honored,
// whatever LidSwitchIgnoreInhibited says; HoldoffTimeoutSec still applies.
void FreedesktopWakeInhibitor::acquireLid(const QString& reason) {
    if (lidFd_.isValid()) { return; }
    lidFd_ = inhibit_(lidLock(), reason);
    if (!lidFd_.isValid()) {
        qCWarning(lcDishWake) << "no handle-lid-switch lock; closing the lid will still suspend";
    }
}

void FreedesktopWakeInhibitor::releaseLid() { lidFd_ = QDBusUnixFileDescriptor(); }

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
