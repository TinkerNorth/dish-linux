// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// TouchpadModeRepository — durable per-satellite touchpad-routing picks
// ("off" | "ds4" | "mouse", held as the wire strings so they round-trip with no
// mapping shim), stored as ONE JSON array under "touchpad_mode_preferences".
// Wrap in TouchpadModeStore for reactive reads.
//
// Invariants:
//   * get() on a satellite NEVER written returns std::nullopt, not a default
//     mode. The layers above collapse absence to the pair-time default, so the
//     repo has to stay honest about "never picked" vs "explicitly off".
//   * put() of an UNKNOWN mode is rejected at the door and leaves an existing
//     valid pick alone, so a typo never persists a mode the satellite cannot
//     route.
//   * Corrupt JSON falls back to an EMPTY list — losing picks beats bricking
//     startup on a garbled blob.

#pragma once

#include "architecture/Repository.h"
#include "core/reducer/TouchpadModeResolve.h"

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

struct TouchpadModePreference {
    QString satelliteId;
    QString mode;

    bool operator==(const TouchpadModePreference& o) const {
        return satelliteId == o.satelliteId && mode == o.mode;
    }
    bool operator!=(const TouchpadModePreference& o) const { return !(*this == o); }
};

// One key holds the whole list; each element is {k:<storageKey>,
// sat:<value.satelliteId>, mode:<value.mode>}. The storage key is kept
// independent of the value's own satelliteId so put(key, value)/get(key)
// round-trip verbatim even when they differ — the Map<K,V> faithfulness
// RepositoryContract pins.
inline constexpr const char* kTouchpadModeListKey = "touchpad_mode_preferences";

class TouchpadModeRepository : public arch::KeyedRepository<QString, TouchpadModePreference> {
  public:
    // nullptr -> the default HKCU store; tests inject their own.
    explicit TouchpadModeRepository(std::shared_ptr<QSettings> settings = nullptr)
        : settings_(settings ? std::move(settings)
                             : std::make_shared<QSettings>(QStringLiteral("Dish"),
                                                           QStringLiteral("Dish"))) {}

    QString keyOf(const TouchpadModePreference& value) const override { return value.satelliteId; }

    std::optional<TouchpadModePreference> get(const QString& satelliteId) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& r : readRows()) {
            if (r.key == satelliteId) { return r.value; }
        }
        return std::nullopt; // honest "never picked"
    }

    std::vector<TouchpadModePreference> all() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TouchpadModePreference> out;
        for (const auto& r : readRows()) { out.push_back(r.value); }
        return out;
    }

    void put(const QString& satelliteId, const TouchpadModePreference& value) override {
        // Reject unknown modes so a typo never persists a value the satellite
        // cannot route.
        if (!reducer::isValidTouchpadModeName(value.mode.toStdString())) { return; }
        std::lock_guard<std::mutex> lock(mutex_);
        auto rows = readRows();
        // Replace in place so the list never grows on a repeat put for one key.
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Row& r) { return r.key == satelliteId; }),
                   rows.end());
        rows.push_back(Row{satelliteId, value});
        writeRows(rows);
    }

    void remove(const QString& satelliteId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto rows = readRows();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Row& r) { return r.key == satelliteId; }),
                   rows.end());
        writeRows(rows);
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_->remove(QLatin1String(kTouchpadModeListKey));
    }

    // Un-hide the KeyedRepository value-overloads the declarations above shadow.
    using arch::KeyedRepository<QString, TouchpadModePreference>::put;
    using arch::KeyedRepository<QString, TouchpadModePreference>::removeValue;

  private:
    struct Row {
        QString key;
        TouchpadModePreference value;
    };

    // Both assume mutex_ is held.
    std::vector<Row> readRows() const {
        const auto raw = settings_->value(QLatin1String(kTouchpadModeListKey)).toByteArray();
        if (raw.isEmpty()) { return {}; }
        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(raw, &err);
        // Corrupt or non-array blob -> empty; garbled prefs must not crash.
        if (err.error != QJsonParseError::NoError || !doc.isArray()) { return {}; }

        std::vector<Row> out;
        for (const auto& v : doc.array()) {
            if (!v.isObject()) { continue; }
            const auto obj = v.toObject();
            const QString sat = obj.value(QLatin1String("sat")).toString();
            const QString key =
                obj.contains(QLatin1String("k")) ? obj.value(QLatin1String("k")).toString() : sat;
            if (key.isEmpty()) { continue; }
            out.push_back(
                Row{key, TouchpadModePreference{sat, obj.value(QLatin1String("mode")).toString()}});
        }
        return out;
    }

    void writeRows(const std::vector<Row>& rows) {
        QJsonArray arr;
        for (const auto& r : rows) {
            arr.append(
                QJsonObject{{"k", r.key}, {"sat", r.value.satelliteId}, {"mode", r.value.mode}});
        }
        settings_->setValue(QLatin1String(kTouchpadModeListKey),
                            QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
