// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/PowerStatus.h"

#include <QDir>
#include <QFile>
#include <QStringList>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dish::source {

namespace {

// A sysfs attribute's one line, empty when the file is missing or unreadable.
std::optional<QString> attribute(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { return std::nullopt; }
    return QString::fromUtf8(file.readAll()).trimmed();
}

std::string attributeText(const QString& path) {
    return attribute(path).value_or(QString()).toStdString();
}

std::optional<std::int64_t> attributeNumber(const QString& path) {
    const std::optional<QString> text = attribute(path);
    if (!text.has_value()) { return std::nullopt; }
    bool ok = false;
    const qlonglong value = text->toLongLong(&ok);
    if (!ok) { return std::nullopt; }
    return static_cast<std::int64_t>(value);
}

// The folder's entries by name, so "BAT0" comes before "BAT1".
QStringList entriesOf(const QString& root, const QString& nameFilter) {
    const QDir dir(root);
    return dir.entryList(QStringList{nameFilter}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::System,
                         QDir::Name);
}

diagnostics::PowerSupplyReading supplyAt(const QString& dir) {
    diagnostics::PowerSupplyReading supply;
    supply.type = attributeText(dir + QStringLiteral("/type"));
    supply.scope = attributeText(dir + QStringLiteral("/scope"));
    supply.status = attributeText(dir + QStringLiteral("/status"));
    const std::optional<std::int64_t> capacity = attributeNumber(dir + QStringLiteral("/capacity"));
    if (capacity.has_value()) { supply.capacity = static_cast<int>(*capacity); }
    return supply;
}

// trip_point_0_type, trip_point_1_type, ... until the first one that is not there.
std::vector<diagnostics::ThermalTrip> tripsAt(const QString& dir) {
    std::vector<diagnostics::ThermalTrip> trips;
    for (int i = 0;; ++i) {
        const QString stem = dir + QStringLiteral("/trip_point_%1_").arg(i);
        const std::optional<QString> type = attribute(stem + QStringLiteral("type"));
        if (!type.has_value()) { break; }
        const std::optional<std::int64_t> temp = attributeNumber(stem + QStringLiteral("temp"));
        if (!temp.has_value()) { continue; }
        trips.push_back(diagnostics::ThermalTrip{type->toStdString(), *temp});
    }
    return trips;
}

} // namespace

diagnostics::PowerFacts readPowerFacts(const QString& powerSupplyRoot,
                                       const QString& platformProfilePath) {
    std::vector<diagnostics::PowerSupplyReading> supplies;
    for (const QString& name : entriesOf(powerSupplyRoot, QStringLiteral("*"))) {
        supplies.push_back(supplyAt(powerSupplyRoot + QLatin1Char('/') + name));
    }
    std::optional<std::string> profile;
    if (const std::optional<QString> text = attribute(platformProfilePath); text.has_value()) {
        profile = text->toStdString();
    }
    return diagnostics::powerFactsOf(supplies, profile);
}

std::optional<diagnostics::ThermalState> readThermalState(const QString& thermalRoot) {
    std::vector<diagnostics::ThermalZoneReading> zones;
    for (const QString& name : entriesOf(thermalRoot, QStringLiteral("thermal_zone*"))) {
        const QString dir = thermalRoot + QLatin1Char('/') + name;
        diagnostics::ThermalZoneReading zone;
        zone.milliCelsius = attributeNumber(dir + QStringLiteral("/temp"));
        zone.trips = tripsAt(dir);
        zones.push_back(std::move(zone));
    }
    return diagnostics::thermalStateOf(zones);
}

} // namespace dish::source
