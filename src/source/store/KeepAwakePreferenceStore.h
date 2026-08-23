// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// KeepAwakePreferenceStore — how far Dish may hold the machine awake while it
// streams. Follows the BackgroundPreferenceStore shape: scalar prefs straight
// over QSettings, no keyed Repository and so no RepositoryContract.

#pragma once

#include "architecture/StateSource.h"
#include "core/reducer/KeepAwake.h"
#include "repository/AppSettings.h"

#include <QLatin1String>
#include <QSettings>
#include <QString>
#include <QVariant>

#include <memory>
#include <string>
#include <string_view>

namespace dish::source {

class KeepAwakePreferenceStore : public arch::StateSource<reducer::KeepAwakePreferences> {
  public:
    static constexpr const char* kKeyMode = "keep_awake_mode";
    static constexpr const char* kKeyTimeoutMinutes = "keep_awake_timeout_minutes";
    static constexpr const char* kKeyDisplay = "keep_awake_display";

    KeepAwakePreferenceStore() : KeepAwakePreferenceStore(repository::makeSettings()) {}

    explicit KeepAwakePreferenceStore(std::unique_ptr<QSettings> settings)
        : arch::StateSource<reducer::KeepAwakePreferences>(readInitial(*settings)),
          settings_(std::move(settings)) {}

    reducer::KeepAwakeMode mode() const { return state().value().mode; }
    int idleTimeoutMinutes() const { return state().value().idleTimeoutMinutes; }
    bool keepDisplayAwake() const { return state().value().keepDisplayAwake; }

    // All three persist + republish and are idempotent: a repeat set does not
    // re-emit.
    void setMode(reducer::KeepAwakeMode mode) {
        reducer::KeepAwakePreferences next = state().value();
        if (next.mode == mode) { return; }
        next.mode = mode;
        const std::string_view key = reducer::keepAwakeModeKey(mode);
        write(kKeyMode, QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())));
        setState(next);
    }

    void setIdleTimeoutMinutes(int minutes) {
        const int clamped = reducer::clampKeepAwakeTimeoutMinutes(minutes);
        reducer::KeepAwakePreferences next = state().value();
        if (next.idleTimeoutMinutes == clamped) { return; }
        next.idleTimeoutMinutes = clamped;
        write(kKeyTimeoutMinutes, clamped);
        setState(next);
    }

    void setKeepDisplayAwake(bool enabled) {
        reducer::KeepAwakePreferences next = state().value();
        if (next.keepDisplayAwake == enabled) { return; }
        next.keepDisplayAwake = enabled;
        write(kKeyDisplay, enabled);
        setState(next);
    }

  private:
    void write(const char* key, const QVariant& value) {
        settings_->setValue(QLatin1String(key), value);
        settings_->sync();
    }

    static reducer::KeepAwakePreferences readInitial(QSettings& settings) {
        reducer::KeepAwakePreferences initial;
        const std::string modeKey =
            settings.value(QLatin1String(kKeyMode)).toString().toStdString();
        if (!modeKey.empty()) { initial.mode = reducer::keepAwakeModeFromKey(modeKey); }
        initial.idleTimeoutMinutes = reducer::clampKeepAwakeTimeoutMinutes(
            settings
                .value(QLatin1String(kKeyTimeoutMinutes), reducer::kKeepAwakeDefaultTimeoutMinutes)
                .toInt());
        initial.keepDisplayAwake = settings.value(QLatin1String(kKeyDisplay), false).toBool();
        return initial;
    }

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
