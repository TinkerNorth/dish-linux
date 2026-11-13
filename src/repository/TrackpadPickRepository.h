// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// TrackpadPickRepository — durable per-slot trackpad picks for the Steam
// Controllers ("stick" | "pad" | "mouse" | "off", the ladder's own strings so
// they round-trip with no mapping shim), stored as ONE JSON array under
// "trackpad_pick_preferences". Wrap in TrackpadPickStore for reactive reads.
//
// Invariants, the same as TouchpadModeRepository's:
//   * get() on a slot NEVER written returns std::nullopt, not a default pick:
//     the ladder owns the default, per model.
//   * put() of an UNKNOWN pick is rejected at the door and leaves an existing
//     valid pick alone.
//   * Corrupt JSON falls back to an EMPTY list.

#pragma once

#include "architecture/Repository.h"
#include "core/reducer/TrackpadPickResolve.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dish::repository {

struct TrackpadPickPreference {
    QString slotId;
    QString pick;

    bool operator==(const TrackpadPickPreference& o) const {
        return slotId == o.slotId && pick == o.pick;
    }
    bool operator!=(const TrackpadPickPreference& o) const { return !(*this == o); }
};

// One key holds the whole list; each element is {k:<storageKey>, slot:<value.slotId>,
// pick:<value.pick>}. The storage key is kept independent of the value's own slotId so
// put(key, value)/get(key) round-trip verbatim even when they differ, the Map<K,V>
// faithfulness RepositoryContract pins.
inline constexpr const char* kTrackpadPickListKey = "trackpad_pick_preferences";

class TrackpadPickRepository : public arch::KeyedRepository<QString, TrackpadPickPreference> {
  public:
    // nullptr -> the default store; tests inject their own.
    explicit TrackpadPickRepository(std::shared_ptr<QSettings> settings = nullptr)
        : settings_(settings ? std::move(settings)
                             : std::make_shared<QSettings>(QStringLiteral("Dish"),
                                                           QStringLiteral("Dish"))) {}

    QString keyOf(const TrackpadPickPreference& value) const override { return value.slotId; }

    std::optional<TrackpadPickPreference> get(const QString& slotId) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& r : readRows()) {
            if (r.key == slotId) { return r.value; }
        }
        return std::nullopt;
    }

    std::vector<TrackpadPickPreference> all() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TrackpadPickPreference> out;
        for (const auto& r : readRows()) { out.push_back(r.value); }
        return out;
    }

    void put(const QString& slotId, const TrackpadPickPreference& value) override {
        if (!reducer::isValidTrackpadPickName(value.pick.toStdString())) { return; }
        std::lock_guard<std::mutex> lock(mutex_);
        auto rows = readRows();
        rows.erase(
            std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == slotId; }),
            rows.end());
        rows.push_back(Row{slotId, value});
        writeRows(rows);
    }

    void remove(const QString& slotId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto rows = readRows();
        rows.erase(
            std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == slotId; }),
            rows.end());
        writeRows(rows);
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_->remove(QLatin1String(kTrackpadPickListKey));
    }

    // Un-hide the KeyedRepository value-overloads the declarations above shadow.
    using arch::KeyedRepository<QString, TrackpadPickPreference>::put;
    using arch::KeyedRepository<QString, TrackpadPickPreference>::removeValue;

  private:
    struct Row {
        QString key;
        TrackpadPickPreference value;
    };

    // Both assume mutex_ is held.
    std::vector<Row> readRows() const {
        const auto raw = settings_->value(QLatin1String(kTrackpadPickListKey)).toByteArray();
        if (raw.isEmpty()) { return {}; }
        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(raw, &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray()) { return {}; }
        std::vector<Row> out;
        for (const auto& v : doc.array()) {
            if (!v.isObject()) { continue; }
            const auto obj = v.toObject();
            const QString key = obj.value(QLatin1String("k")).toString();
            const QString pick = obj.value(QLatin1String("pick")).toString();
            const bool usable =
                !key.isEmpty() && reducer::isValidTrackpadPickName(pick.toStdString());
            if (!usable) { continue; }
            out.push_back(Row{
                key, TrackpadPickPreference{obj.value(QLatin1String("slot")).toString(), pick}});
        }
        return out;
    }

    void writeRows(const std::vector<Row>& rows) {
        QJsonArray arr;
        for (const auto& r : rows) {
            arr.append(QJsonObject{{QLatin1String("k"), r.key},
                                   {QLatin1String("slot"), r.value.slotId},
                                   {QLatin1String("pick"), r.value.pick}});
        }
        settings_->setValue(QLatin1String(kTrackpadPickListKey), QJsonDocument(arr).toJson());
        settings_->sync();
    }

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
