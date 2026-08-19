// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/BluetoothRadioProbe.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QVariant>

namespace dish::source {

namespace {

constexpr auto kBluezService = "org.bluez";
constexpr auto kAdapterInterface = "org.bluez.Adapter1";

bool anyAdapterPowered() {
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) { return false; }
    // Adapter object paths are /org/bluez/hciN, so the sysfs names map straight
    // across and no ObjectManager round trip is needed.
    const QDir sysfs(QStringLiteral("/sys/class/bluetooth"));
    for (const QString& hci : sysfs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDBusInterface adapter(QLatin1String(kBluezService), QStringLiteral("/org/bluez/") + hci,
                               QLatin1String(kAdapterInterface), bus);
        if (!adapter.isValid()) { continue; }
        const QVariant powered = adapter.property("Powered");
        if (powered.isValid() && powered.toBool()) { return true; }
    }
    return false;
}

} // namespace

BluetoothRadioState probeBluetoothRadio() {
    BluetoothRadioState state;
    const QDir sysfs(QStringLiteral("/sys/class/bluetooth"));
    state.present = sysfs.exists() && !sysfs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    state.enabled = state.present && anyAdapterPowered();
    return state;
}

} // namespace dish::source
