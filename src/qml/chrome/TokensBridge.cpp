// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/chrome/TokensBridge.h"

#include "UI/FontStacks.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

#include <optional>

namespace dish::chrome {

namespace {

// The XDG settings portal proxies the desktop's own key, which is where GNOME
// and anything that mirrors its schema publish the preference.
std::optional<bool> portalAnimationsEnabled() {
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("Read"));
    call << QStringLiteral("org.gnome.desktop.interface") << QStringLiteral("enable-animations");
    const QDBusMessage reply =
        QDBusConnection::sessionBus().call(call, QDBus::Block, /*timeoutMs=*/300);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return std::nullopt;
    }
    const QVariant value = reply.arguments().first().value<QDBusVariant>().variant();
    if (!value.isValid()) { return std::nullopt; }
    return value.toBool();
}

std::optional<bool> kdeAnimationsEnabled() {
    const QString config =
        QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    if (config.isEmpty()) { return std::nullopt; }
    QSettings globals(config, QSettings::IniFormat);
    return kdeAnimationsEnabledFor(globals.value(QStringLiteral("KDE/AnimationDurationFactor")));
}

bool probeReducedMotion() {
    return reducedMotionFrom(portalAnimationsEnabled, kdeAnimationsEnabled);
}

} // namespace

TokensBridge::TokensBridge(QObject* parent) : QObject(parent), motion_(probeReducedMotion()) {}

QString TokensBridge::monoFamily() const { return ui::preferredMonoFamily(); }

QString TokensBridge::sansFamily() const { return ui::preferredSansFamily(); }

void TokensBridge::refreshMotionPreference() {
    if (motion_.update(probeReducedMotion())) { emit reducedMotionChanged(); }
}

} // namespace dish::chrome
