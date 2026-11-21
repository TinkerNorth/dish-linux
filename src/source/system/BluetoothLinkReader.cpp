// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/BluetoothLinkReader.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cstddef>

namespace dish::source {

namespace {

constexpr auto kBluezService = "org.bluez";
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto kDeviceInterface = "org.bluez.Device1";
// The support file waits on this at most once per Bluetooth pad that uhid made.
constexpr int kBluezTimeoutMs = 500;

bool isAdapterName(const QString& name) {
    const QLatin1String prefix("hci");
    const QString number = name.mid(prefix.size());
    const bool numbered = !number.isEmpty() && std::all_of(number.begin(), number.end(),
                                                           [](QChar ch) { return ch.isDigit(); });
    return name.startsWith(prefix) && numbered;
}

// The adapters, hci0 and on; sysfs lists each connection (hci0:256) beside them.
QStringList adapterNames() {
    const QDir sysfs(QStringLiteral("/sys/class/bluetooth"));
    QStringList adapters;
    for (const QString& name : sysfs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (isAdapterName(name)) { adapters.append(name); }
    }
    return adapters;
}

// BlueZ names a device's object path from its address: /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF.
QString devicePath(const QString& adapter, const std::string& address) {
    QString name = QString::fromStdString(address);
    name.replace(QLatin1Char(':'), QLatin1Char('_'));
    return QStringLiteral("/org/bluez/") + adapter + QStringLiteral("/dev_") + name;
}

std::optional<std::vector<std::string>> servicesAt(QDBusConnection& bus, const QString& path) {
    QDBusMessage get =
        QDBusMessage::createMethodCall(QLatin1String(kBluezService), path,
                                       QLatin1String(kPropertiesInterface), QStringLiteral("Get"));
    get << QLatin1String(kDeviceInterface) << QStringLiteral("UUIDs");
    const QDBusMessage reply = bus.call(get, QDBus::Block, kBluezTimeoutMs);
    const bool answered =
        reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty();
    if (!answered) { return std::nullopt; }
    const QStringList uuids =
        reply.arguments().constFirst().value<QDBusVariant>().variant().toStringList();
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(uuids.size()));
    for (const QString& uuid : uuids) { out.push_back(uuid.toStdString()); }
    return out;
}

} // namespace

std::optional<std::vector<std::string>> bluezServiceUuids(const std::string& address) {
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) { return std::nullopt; }
    for (const QString& adapter : adapterNames()) {
        const auto services = servicesAt(bus, devicePath(adapter, address));
        if (services.has_value()) { return services; }
    }
    return std::nullopt;
}

input::TransportLayer readTransportLayer(input::BluetoothHidOrigin origin,
                                         const std::string& hidNode) {
    if (origin != input::BluetoothHidOrigin::Uhid) {
        return input::transportLayerOf(origin, std::nullopt);
    }
    const std::optional<std::string> address =
        input::remoteAddressOf(input::hidUniqOfHidPath(hidNode));
    if (!address.has_value()) { return input::TransportLayer::Unknown; }
    return input::transportLayerOf(origin, bluezServiceUuids(*address));
}

} // namespace dish::source
