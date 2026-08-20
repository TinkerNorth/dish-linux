// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// UiPreferenceStore — small persisted UI-shell preferences (currently the
// navigation rail's collapsed state), republished reactively so the shell
// re-renders on a flip. Desktop-only: the collapsible rail is a desktop
// shell concept with no android analog, but the store follows the exact
// OnboardingPreferenceStore shape (scalar prefs live directly in the Source
// over QSettings — no keyed Repository, hence no RepositoryContract).

#pragma once

#include "architecture/StateSource.h"

#include <QSettings>

#include <memory>

namespace dish::source {

// ==-comparable so distinct-until-changed suppresses no-op re-emits.
struct UiPreferences {
    bool railCollapsed = false;

    bool operator==(const UiPreferences& o) const { return railCollapsed == o.railCollapsed; }
    bool operator!=(const UiPreferences& o) const { return !(*this == o); }
};

class UiPreferenceStore : public arch::StateSource<UiPreferences> {
  public:
    static constexpr const char* kKeyRailCollapsed = "ui_rail_collapsed";

    UiPreferenceStore() : UiPreferenceStore(std::make_unique<QSettings>()) {}

    explicit UiPreferenceStore(std::unique_ptr<QSettings> settings)
        : arch::StateSource<UiPreferences>(readInitial(*settings)), settings_(std::move(settings)) {
    }

    bool railCollapsed() const { return state().value().railCollapsed; }

    // Persist + republish; idempotent (a repeat set does not re-emit).
    void setRailCollapsed(bool collapsed) {
        UiPreferences next = state().value();
        if (next.railCollapsed == collapsed) { return; }
        next.railCollapsed = collapsed;
        settings_->setValue(QLatin1String(kKeyRailCollapsed), collapsed);
        settings_->sync();
        setState(next);
    }

  private:
    static UiPreferences readInitial(QSettings& settings) {
        UiPreferences initial;
        initial.railCollapsed = settings.value(QLatin1String(kKeyRailCollapsed), false).toBool();
        return initial;
    }

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
