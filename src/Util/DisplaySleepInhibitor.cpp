// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "DisplaySleepInhibitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QLoggingCategory>

namespace dish::util {

namespace {

Q_LOGGING_CATEGORY(lcDishWake, "dish.wake")

QString screenSaverService() { return QStringLiteral("org.freedesktop.ScreenSaver"); }
QString screenSaverPath() { return QStringLiteral("/org/freedesktop/ScreenSaver"); }

} // namespace

FreedesktopScreenSaverInhibitor::FreedesktopScreenSaverInhibitor(QObject* parent)
    : DisplaySleepInhibitor(parent) {}

FreedesktopScreenSaverInhibitor::~FreedesktopScreenSaverInhibitor() {
    // RAII: never leak a cookie even if the AppModel forgot to release. The
    // session bus disappearing on logout is also fine — DBus drops cookies
    // tied to dead peers automatically.
    if (cookie_.has_value()) {
        QDBusInterface iface(screenSaverService(), screenSaverPath(), screenSaverService(),
                             QDBusConnection::sessionBus());
        iface.call(QStringLiteral("UnInhibit"), *cookie_);
    }
}

void FreedesktopScreenSaverInhibitor::acquire(const QString& reason) {
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

void FreedesktopScreenSaverInhibitor::release() {
    if (!cookie_.has_value()) { return; }
    QDBusInterface iface(screenSaverService(), screenSaverPath(), screenSaverService(),
                         QDBusConnection::sessionBus());
    if (iface.isValid()) { iface.call(QStringLiteral("UnInhibit"), *cookie_); }
    qCDebug(lcDishWake) << "ScreenSaver.UnInhibit cookie=" << *cookie_;
    cookie_.reset();
}

} // namespace dish::util
