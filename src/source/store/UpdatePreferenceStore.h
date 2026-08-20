// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// UpdatePreferenceStore — the two reactive update preferences, republished so
// the checker and the Settings page re-render on a flip. Follows the
// UiPreferenceStore shape (scalar prefs directly in the Source over QSettings,
// no keyed Repository, hence no RepositoryContract).
//
// There is no auto-download preference: Dish never installs its own update on
// Linux, so the only choice is whether to look.

#pragma once

#include "architecture/StateSource.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <utility>

namespace dish::source {

// ==-comparable so distinct-until-changed suppresses no-op re-emits.
struct UpdatePreferences {
    bool checksEnabled = true;
    // The exact version the user muted; "" when none. Cleared by a newer
    // release, ignored while the running build is below the supported minimum.
    QString skippedVersion;

    bool operator==(const UpdatePreferences& o) const {
        return checksEnabled == o.checksEnabled && skippedVersion == o.skippedVersion;
    }
    bool operator!=(const UpdatePreferences& o) const { return !(*this == o); }
};

// Imperative bookkeeping: shares the backing store but not the reactive slice,
// because nothing re-renders on it.
inline constexpr const char* kKeyUpdatesLastCheckUtcMs = "updates_last_check_utc_ms";

class UpdatePreferenceStore : public arch::StateSource<UpdatePreferences> {
  public:
    static constexpr const char* kKeyChecksEnabled = "updates_check_enabled";
    static constexpr const char* kKeySkippedVersion = "updates_skipped_version";

    UpdatePreferenceStore() : UpdatePreferenceStore(std::make_unique<QSettings>()) {}

    explicit UpdatePreferenceStore(std::unique_ptr<QSettings> settings)
        : arch::StateSource<UpdatePreferences>(readInitial(*settings)),
          settings_(std::move(settings)) {}

    bool checksEnabled() const { return state().value().checksEnabled; }
    QString skippedVersion() const { return state().value().skippedVersion; }

    // Both persist + republish and are idempotent (a repeat set does not
    // re-emit, so the checker cannot loop through its own subscription).
    void setChecksEnabled(bool enabled) {
        UpdatePreferences next = state().value();
        if (next.checksEnabled == enabled) { return; }
        next.checksEnabled = enabled;
        settings_->setValue(QLatin1String(kKeyChecksEnabled), enabled);
        settings_->sync();
        setState(next);
    }

    void setSkippedVersion(const QString& version) {
        UpdatePreferences next = state().value();
        if (next.skippedVersion == version) { return; }
        next.skippedVersion = version;
        settings_->setValue(QLatin1String(kKeySkippedVersion), version);
        settings_->sync();
        setState(next);
    }

  private:
    static UpdatePreferences readInitial(QSettings& settings) {
        UpdatePreferences initial;
        initial.checksEnabled = settings.value(QLatin1String(kKeyChecksEnabled), true).toBool();
        initial.skippedVersion =
            settings.value(QLatin1String(kKeySkippedVersion), QString()).toString();
        return initial;
    }

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
