// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// ControllerDbPreferenceStore — "Check for controller updates automatically"
// (docs/controller-db.md section 6.4), default on, republished so the Settings
// switch and the check coordinator follow a flip, plus the imperative
// bookkeeping of the last check: when it ran, how it ended, and the ETag of the
// manifest it read with the database version that ETag belongs to. The
// UpdatePreferenceStore shape, in the same settings file.
//
// Unlike the update check, the default is on inside a Flatpak too: the
// contract asks for the switch on in every build and store flavour, and
// Flathub delivers the app, not the controller list.

#pragma once

#include "architecture/StateSource.h"

#include <QByteArray>
#include <QSettings>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace dish::source {

struct ControllerDbPreferences {
    bool checksEnabled = true;

    bool operator==(const ControllerDbPreferences&) const = default;
};

// What the last check came to, persisted so Diagnostics can show it across runs.
struct ControllerDbLastCheck {
    std::int64_t atMs = 0;
    QString result; // a core/controllerdb CheckResult token, "" when never checked

    bool operator==(const ControllerDbLastCheck&) const = default;
};

class ControllerDbPreferenceStore : public arch::StateSource<ControllerDbPreferences> {
  public:
    static constexpr const char* kKeyChecksEnabled = "controller_db_check_enabled";
    static constexpr const char* kKeyLastCheckMs = "controller_db_last_check_utc_ms";
    static constexpr const char* kKeyLastCheckResult = "controller_db_last_check_result";
    static constexpr const char* kKeyEtag = "controller_db_manifest_etag";
    static constexpr const char* kKeyEtagVersion = "controller_db_manifest_etag_version";

    ControllerDbPreferenceStore() : ControllerDbPreferenceStore(std::make_unique<QSettings>()) {}

    explicit ControllerDbPreferenceStore(std::unique_ptr<QSettings> settings)
        : arch::StateSource<ControllerDbPreferences>(readInitial(*settings)),
          settings_(std::move(settings)) {}

    bool checksEnabled() const { return state().value().checksEnabled; }

    void setChecksEnabled(bool enabled) {
        ControllerDbPreferences next = state().value();
        if (next.checksEnabled == enabled) { return; }
        next.checksEnabled = enabled;
        settings_->setValue(QLatin1String(kKeyChecksEnabled), enabled);
        settings_->sync();
        setState(next);
    }

    ControllerDbLastCheck lastCheck() const {
        return ControllerDbLastCheck{
            settings_->value(QLatin1String(kKeyLastCheckMs), qint64(0)).toLongLong(),
            settings_->value(QLatin1String(kKeyLastCheckResult), QString()).toString()};
    }

    void setLastCheck(const ControllerDbLastCheck& check) {
        settings_->setValue(QLatin1String(kKeyLastCheckMs), qint64(check.atMs));
        settings_->setValue(QLatin1String(kKeyLastCheckResult), check.result);
        settings_->sync();
    }

    QByteArray etag() const {
        return settings_->value(QLatin1String(kKeyEtag), QByteArray()).toByteArray();
    }
    int etagVersion() const { return settings_->value(QLatin1String(kKeyEtagVersion), 0).toInt(); }

    void setEtag(const QByteArray& etag, int forVersion) {
        settings_->setValue(QLatin1String(kKeyEtag), etag);
        settings_->setValue(QLatin1String(kKeyEtagVersion), forVersion);
        settings_->sync();
    }

  private:
    static ControllerDbPreferences readInitial(QSettings& settings) {
        ControllerDbPreferences initial;
        initial.checksEnabled = settings.value(QLatin1String(kKeyChecksEnabled), true).toBool();
        return initial;
    }

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
