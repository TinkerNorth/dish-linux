// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/notification/DesktopNotifier.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QLoggingCategory>
#include <QStringList>
#include <QVariantMap>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcDishNotify, "dish.notify")

constexpr int kNotificationTimeoutMs = 6000;

} // namespace

void FreedesktopNotifier::notify(const QString& summary, const QString& body) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCDebug(lcDishNotify) << "no session bus; notification dropped";
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("com.tinkernorth.Dish"));
    call << QStringLiteral("Dish") << 0U << QStringLiteral("com.tinkernorth.Dish") << summary
         << body << QStringList() << hints << kNotificationTimeoutMs;
    // Fire and forget: a desktop with no notification daemon is not an error,
    // and the reply carries nothing anyone here reads.
    if (!bus.send(call)) { qCDebug(lcDishNotify) << "notification send failed"; }
}

} // namespace dish::source
