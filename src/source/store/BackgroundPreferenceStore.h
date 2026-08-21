// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// BackgroundPreferenceStore — whether closing the window leaves Dish running
// behind a tray item, and whether the user has been told that it does. Follows
// the UiPreferenceStore shape: a scalar pref straight over QSettings, no keyed
// Repository and so no RepositoryContract.
//
// kDefaultRunInBackground is true, but the preference alone never hides a
// window: reducer::decideCloseAction also requires a tray host, so a desktop
// without one keeps quitting on close.

#pragma once

#include "architecture/StateSource.h"

#include <QSettings>

#include <memory>

namespace dish::source {

// ==-comparable so distinct-until-changed suppresses no-op re-emits.
struct BackgroundPreferences {
    bool runInBackground = true;
    bool noticeShown = false;

    bool operator==(const BackgroundPreferences& o) const {
        return runInBackground == o.runInBackground && noticeShown == o.noticeShown;
    }
    bool operator!=(const BackgroundPreferences& o) const { return !(*this == o); }
};

class BackgroundPreferenceStore : public arch::StateSource<BackgroundPreferences> {
  public:
    static constexpr const char* kKeyRunInBackground = "background_run_enabled";
    static constexpr const char* kKeyNoticeShown = "background_notice_shown";
    static constexpr bool kDefaultRunInBackground = true;

    BackgroundPreferenceStore() : BackgroundPreferenceStore(std::make_unique<QSettings>()) {}

    explicit BackgroundPreferenceStore(std::unique_ptr<QSettings> settings)
        : arch::StateSource<BackgroundPreferences>(readInitial(*settings)),
          settings_(std::move(settings)) {}

    bool runInBackground() const { return state().value().runInBackground; }
    bool noticeShown() const { return state().value().noticeShown; }

    // Persist + republish; idempotent (a repeat set does not re-emit).
    void setRunInBackground(bool enabled) {
        BackgroundPreferences next = state().value();
        if (next.runInBackground == enabled) { return; }
        next.runInBackground = enabled;
        settings_->setValue(QLatin1String(kKeyRunInBackground), enabled);
        settings_->sync();
        setState(next);
    }

    void setNoticeShown(bool shown) {
        BackgroundPreferences next = state().value();
        if (next.noticeShown == shown) { return; }
        next.noticeShown = shown;
        settings_->setValue(QLatin1String(kKeyNoticeShown), shown);
        settings_->sync();
        setState(next);
    }

  private:
    static BackgroundPreferences readInitial(QSettings& settings) {
        BackgroundPreferences initial;
        initial.runInBackground =
            settings.value(QLatin1String(kKeyRunInBackground), kDefaultRunInBackground).toBool();
        initial.noticeShown = settings.value(QLatin1String(kKeyNoticeShown), false).toBool();
        return initial;
    }

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
