// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/SleepMonitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QLoggingCategory>
#include <QStringLiteral>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcDishSleep, "dish.sleep")

QString logindService() { return QStringLiteral("org.freedesktop.login1"); }
QString logindPath() { return QStringLiteral("/org/freedesktop/login1"); }
QString logindManager() { return QStringLiteral("org.freedesktop.login1.Manager"); }

} // namespace

LogindSleepMonitor::LogindSleepMonitor(QObject* parent) : SleepMonitor(parent) {}

LogindSleepMonitor::~LogindSleepMonitor() { releaseDelay(); }

void LogindSleepMonitor::start() {
    if (started_) { return; }
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCDebug(lcDishSleep) << "no system bus; suspend/resume handling disabled";
        return;
    }
    if (!bus.connect(logindService(), logindPath(), logindManager(),
                     QStringLiteral("PrepareForSleep"), this,
                     SLOT(handlePrepareForSleep(bool)))) {
        qCWarning(lcDishSleep) << "login1 PrepareForSleep subscribe failed";
        return;
    }
    started_ = true;
    acquireDelay();
}

void LogindSleepMonitor::stop() {
    if (!started_) { return; }
    QDBusConnection::systemBus().disconnect(logindService(), logindPath(), logindManager(),
                                            QStringLiteral("PrepareForSleep"), this,
                                            SLOT(handlePrepareForSleep(bool)));
    started_ = false;
    releaseDelay();
}

// Order is the whole point: subscribers tear their sessions down on the emit,
// which is synchronous, and only then does the lock close and the machine go.
void LogindSleepMonitor::handlePrepareForSleep(bool starting) {
    if (starting) {
        emit preparingForSleep(true);
        releaseDelay();
        return;
    }
    acquireDelay();
    emit preparingForSleep(false);
}

void LogindSleepMonitor::acquireDelay() {
    if (delayFd_.isValid()) { return; }
    QDBusInterface iface(logindService(), logindPath(), logindManager(),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        qCDebug(lcDishSleep) << "logind unavailable; no suspend delay lock";
        return;
    }
    const QDBusReply<QDBusUnixFileDescriptor> reply =
        iface.call(QStringLiteral("Inhibit"), QStringLiteral("sleep"), QStringLiteral("Dish"),
                   QStringLiteral("Closing satellite sessions before suspend"),
                   QStringLiteral("delay"));
    if (!reply.isValid()) {
        qCWarning(lcDishSleep) << "login1 sleep-delay Inhibit failed:" << reply.error().message();
        return;
    }
    delayFd_ = reply.value();
}

void LogindSleepMonitor::releaseDelay() { delayFd_ = QDBusUnixFileDescriptor(); }

} // namespace dish::source
