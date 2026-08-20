// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/DeadzoneRepository.h"

#include "repository/SettingsKeys.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace dish::repository {

namespace {

// One JSON object per device (rather than sub-keys) keeps all()/clear() a pure
// prefix enumeration. The entry's own `deviceId` is stored independently of the
// storage key so get(key) returns exactly what put stored — the Map<K,V>
// faithfulness RepositoryContract pins.
constexpr const char* kFieldDevice = "d";
constexpr const char* kFieldStick = "s";
constexpr const char* kFieldTrigger = "t";

QString storageKey(const QString& deviceId) {
    return QLatin1String(keys::kDeadzonePrefix) + deviceId;
}

// nullopt on a missing or corrupt blob so a garbled value for one device never
// bricks setup. `fallbackDeviceId` is the storage key, used only for older blobs
// that predate the stored `d` field.
std::optional<DeadzoneEntry> decodeEntry(const QString& fallbackDeviceId, const QByteArray& raw) {
    if (raw.isEmpty()) { return std::nullopt; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return std::nullopt; }
    const auto obj = doc.object();
    if (!obj.contains(QLatin1String(kFieldStick)) || !obj.contains(QLatin1String(kFieldTrigger))) {
        return std::nullopt;
    }
    DeadzoneEntry e;
    e.deviceId = obj.contains(QLatin1String(kFieldDevice))
                     ? obj.value(QLatin1String(kFieldDevice)).toString()
                     : fallbackDeviceId;
    e.stickFlat = static_cast<std::int16_t>(obj.value(QLatin1String(kFieldStick)).toInt());
    e.triggerFlat = static_cast<std::uint8_t>(obj.value(QLatin1String(kFieldTrigger)).toInt());
    return e;
}

QByteArray encodeEntry(const DeadzoneEntry& e) {
    QJsonObject obj;
    obj.insert(QLatin1String(kFieldDevice), e.deviceId);
    obj.insert(QLatin1String(kFieldStick), static_cast<int>(e.stickFlat));
    obj.insert(QLatin1String(kFieldTrigger), static_cast<int>(e.triggerFlat));
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace

DeadzoneRepository::DeadzoneRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {
}

std::optional<input::deadzone::Deadzones>
DeadzoneRepository::deadzonesFor(const QString& deviceId) const {
    const auto entry = get(deviceId);
    if (!entry.has_value()) { return std::nullopt; }
    return entry->deadzones();
}

void DeadzoneRepository::setDeadzones(const QString& deviceId,
                                      const input::deadzone::Deadzones& dz) {
    put(deviceId, DeadzoneEntry{deviceId, dz.stickFlat, dz.triggerFlat});
}

std::optional<DeadzoneEntry> DeadzoneRepository::get(const QString& deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto raw = settings_->value(storageKey(deviceId)).toByteArray();
    return decodeEntry(deviceId, raw);
}

std::vector<DeadzoneEntry> DeadzoneRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeadzoneEntry> out;
    const QString prefix = QLatin1String(keys::kDeadzonePrefix);
    for (const auto& key : settings_->allKeys()) {
        if (!key.startsWith(prefix)) { continue; }
        const QString deviceId = key.mid(prefix.size());
        if (auto e = decodeEntry(deviceId, settings_->value(key).toByteArray())) {
            out.push_back(*e);
        }
    }
    return out;
}

void DeadzoneRepository::put(const QString& deviceId, const DeadzoneEntry& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->setValue(storageKey(deviceId), encodeEntry(value));
}

void DeadzoneRepository::remove(const QString& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(storageKey(deviceId));
}

void DeadzoneRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const QString prefix = QLatin1String(keys::kDeadzonePrefix);
    // Only this repo's namespace: co-tenant pins, shared keys and the remembered
    // list must survive. Collect first — removing while iterating allKeys() is
    // unsafe.
    QStringList toRemove;
    for (const auto& key : settings_->allKeys()) {
        if (key.startsWith(prefix)) { toRemove.append(key); }
    }
    for (const auto& key : toRemove) { settings_->remove(key); }
}

} // namespace dish::repository
