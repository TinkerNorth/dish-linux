// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/AppSettings.h"
#include "repository/RumblePreferenceRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace dish::repository {

namespace {

// Each element is {k:<storageKey>, slot:<value.slotId>, on:<bool>}. The storage
// key is kept apart from the value's own slotId so put(key, value)/get(key)
// round-trip verbatim even when they differ, the Map<K,V> faithfulness
// RepositoryContract pins.
constexpr const char* kListKey = "rumble_preferences";
constexpr const char* kFieldKey = "k";
constexpr const char* kFieldSlot = "slot";
constexpr const char* kFieldOn = "on";

struct Row {
    QString key;
    RumblePreference value;
};

// No parse-error branch: fromJson answers an empty array for any document that
// is not one, a corrupt blob included, and an element with no key has nothing
// to file it under.
std::vector<Row> readRows(const QSettings& settings) {
    const auto raw = settings.value(QLatin1String(kListKey)).toByteArray();
    const auto elements = QJsonDocument::fromJson(raw).array();
    std::vector<Row> out;
    for (const auto& element : elements) {
        const auto obj = element.toObject();
        const QString key = obj.value(QLatin1String(kFieldKey)).toString();
        if (key.isEmpty()) { continue; }
        const QString slotId = obj.value(QLatin1String(kFieldSlot)).toString();
        const bool enabled = obj.value(QLatin1String(kFieldOn)).toBool();
        out.push_back(Row{key, RumblePreference{slotId, enabled}});
    }
    return out;
}

void writeRows(QSettings& settings, const std::vector<Row>& rows) {
    QJsonArray arr;
    for (const auto& r : rows) {
        arr.append(QJsonObject{
            {kFieldKey, r.key}, {kFieldSlot, r.value.slotId}, {kFieldOn, r.value.enabled}});
    }
    settings.setValue(QLatin1String(kListKey), QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void eraseKey(std::vector<Row>& rows, const QString& key) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == key; }),
               rows.end());
}

} // namespace

RumblePreferenceRepository::RumblePreferenceRepository() : settings_(repository::makeSettings()) {}

RumblePreferenceRepository::RumblePreferenceRepository(std::shared_ptr<QSettings> settings)
    : settings_(std::move(settings)) {}

std::optional<RumblePreference> RumblePreferenceRepository::get(const QString& slotId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : readRows(*settings_)) {
        if (r.key == slotId) { return r.value; }
    }
    return std::nullopt;
}

std::vector<RumblePreference> RumblePreferenceRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RumblePreference> out;
    for (const auto& r : readRows(*settings_)) { out.push_back(r.value); }
    return out;
}

void RumblePreferenceRepository::put(const QString& slotId, const RumblePreference& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rows = readRows(*settings_);
    eraseKey(rows, slotId);
    rows.push_back(Row{slotId, value});
    writeRows(*settings_, rows);
}

void RumblePreferenceRepository::remove(const QString& slotId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rows = readRows(*settings_);
    eraseKey(rows, slotId);
    writeRows(*settings_, rows);
}

void RumblePreferenceRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(kListKey));
}

} // namespace dish::repository
