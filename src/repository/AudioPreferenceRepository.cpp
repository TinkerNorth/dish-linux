// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/AudioPreferenceRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace dish::repository {

namespace {

// Each element is {k:<storageKey>, slot:<value.slotId>, on:<bool>}. The storage
// key is kept independent of the value's own slotId so put(key, value)/get(key)
// round-trip verbatim even when they differ — the Map<K,V> faithfulness
// RepositoryContract pins. Field names match MotionPreferenceRepository's so a
// future merge of the toggle repos has one shape to migrate.
constexpr const char* kFieldKey = "k";
constexpr const char* kFieldSlot = "slot";
constexpr const char* kFieldOn = "on";

struct Row {
    QString key;
    AudioPreference value;
};

std::vector<Row> readRows(const QSettings& settings, const QString& listKey) {
    const auto raw = settings.value(listKey).toByteArray();
    if (raw.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(raw, &err);
    // Corrupt or non-array blob -> empty; garbled prefs must not crash startup.
    if (err.error != QJsonParseError::NoError || !doc.isArray()) { return {}; }

    std::vector<Row> out;
    for (const auto& v : doc.array()) {
        if (!v.isObject()) { continue; }
        const auto obj = v.toObject();
        const QString slot = obj.value(QLatin1String(kFieldSlot)).toString();
        const QString key = obj.contains(QLatin1String(kFieldKey))
                                ? obj.value(QLatin1String(kFieldKey)).toString()
                                : slot;
        if (key.isEmpty()) { continue; }
        out.push_back(Row{key, AudioPreference{slot, obj.value(QLatin1String(kFieldOn)).toBool()}});
    }
    return out;
}

void writeRows(QSettings& settings, const QString& listKey, const std::vector<Row>& rows) {
    QJsonArray arr;
    for (const auto& r : rows) {
        arr.append(QJsonObject{
            {kFieldKey, r.key}, {kFieldSlot, r.value.slotId}, {kFieldOn, r.value.enabled}});
    }
    settings.setValue(listKey, QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

} // namespace

AudioPreferenceRepository::AudioPreferenceRepository(QString listKey,
                                                     std::shared_ptr<QSettings> settings)
    : listKey_(std::move(listKey)),
      settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {
}

std::optional<AudioPreference> AudioPreferenceRepository::get(const QString& slotId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : readRows(*settings_, listKey_)) {
        if (r.key == slotId) { return r.value; }
    }
    return std::nullopt; // honest "never written"
}

std::vector<AudioPreference> AudioPreferenceRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AudioPreference> out;
    for (const auto& r : readRows(*settings_, listKey_)) { out.push_back(r.value); }
    return out;
}

void AudioPreferenceRepository::put(const QString& slotId, const AudioPreference& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rows = readRows(*settings_, listKey_);
    // Replace in place so the list never grows on a repeat put for one key.
    rows.erase(
        std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == slotId; }),
        rows.end());
    rows.push_back(Row{slotId, value});
    writeRows(*settings_, listKey_, rows);
}

void AudioPreferenceRepository::remove(const QString& slotId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rows = readRows(*settings_, listKey_);
    rows.erase(
        std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == slotId; }),
        rows.end());
    writeRows(*settings_, listKey_, rows);
}

void AudioPreferenceRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(listKey_);
}

} // namespace dish::repository
