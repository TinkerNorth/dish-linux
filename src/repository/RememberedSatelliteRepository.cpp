// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/RememberedSatelliteRepository.h"

#include "repository/SettingsKeys.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace dish::repository {

namespace {

// Keying by the storage key rather than the row's own `id` is what makes this a
// faithful Map<K,V>: put(key, value)/get(key) round-trip verbatim even when the
// value's id differs from the key. RepositoryContract pins that.
constexpr const char* kSchemaKey = "k"; // entry storage key
constexpr const char* kSchemaVal = "v"; // entry row object

} // namespace

RememberedSatelliteRepository::RememberedSatelliteRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {
}

QMap<QString, models::RememberedWifi> RememberedSatelliteRepository::readEntries() const {
    const auto raw = settings_->value(QLatin1String(keys::kSatelliteListKey)).toByteArray();
    if (raw.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError) { return {}; } // corrupt -> empty (don't brick)

    QMap<QString, models::RememberedWifi> entries;
    if (doc.isArray()) {
        // Migrated shape: a plain JSON array of rows keyed by row id.
        for (const auto& v : doc.array()) {
            if (!v.isObject()) { continue; }
            const auto row = models::RememberedWifi::fromJson(v.toObject());
            entries.insert(row.id, row);
        }
        return entries;
    }
    if (doc.isObject()) {
        // Current shape: {k, v} pairs under a wrapper, preserving the storage key
        // independently of the row's id.
        for (const auto& e : doc.object().value(QLatin1String("entries")).toArray()) {
            const auto eo = e.toObject();
            const QString key = eo.value(QLatin1String(kSchemaKey)).toString();
            const auto row =
                models::RememberedWifi::fromJson(eo.value(QLatin1String(kSchemaVal)).toObject());
            if (!key.isEmpty()) { entries.insert(key, row); }
        }
        return entries;
    }
    return {}; // neither array nor object -> empty
}

void RememberedSatelliteRepository::writeEntries(
    const QMap<QString, models::RememberedWifi>& entries) {
    QJsonArray arr;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        arr.append(QJsonObject{{kSchemaKey, it.key()}, {kSchemaVal, it.value().toJson()}});
    }
    const auto doc = QJsonDocument(QJsonObject{{"entries", arr}});
    settings_->setValue(QLatin1String(keys::kSatelliteListKey), doc.toJson(QJsonDocument::Compact));
}

std::optional<models::RememberedWifi> RememberedSatelliteRepository::get(const QString& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entries = readEntries();
    const auto it = entries.constFind(id);
    if (it == entries.constEnd()) { return std::nullopt; }
    return *it;
}

std::vector<models::RememberedWifi> RememberedSatelliteRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entries = readEntries();
    std::vector<models::RememberedWifi> out;
    out.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& v : entries) { out.push_back(v); }
    return out;
}

void RememberedSatelliteRepository::put(const QString& id, const models::RememberedWifi& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entries = readEntries();
    entries.insert(id, value); // storage key authoritative; value stored verbatim
    writeEntries(entries);
}

void RememberedSatelliteRepository::remove(const QString& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entries = readEntries();
    entries.remove(id);
    writeEntries(entries);
}

void RememberedSatelliteRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(keys::kSatelliteListKey));
}

} // namespace dish::repository
