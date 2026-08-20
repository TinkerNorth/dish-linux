// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The first-run "welcome seen" StateSource: two persisted flags, republished
// reactively.
//
// The keys and store name are a cross-client schema shared with the other Dish
// clients, so renaming one is a migration. Both default false so a fresh
// install shows the welcome pager.

#pragma once

#include "architecture/StateSource.h"

#include <memory>

class QSettings;

namespace dish::source {

// The two onboarding flags. A small ==-comparable value so the Observable's
// distinct-until-changed suppresses no-op re-emits. Mirrors android's
// OnboardingState data class.
struct OnboardingState {
    bool welcomeCompleted = false;
    bool dashboardHintDismissed = false;

    bool operator==(const OnboardingState& o) const {
        return welcomeCompleted == o.welcomeCompleted &&
               dashboardHintDismissed == o.dashboardHintDismissed;
    }
    bool operator!=(const OnboardingState& o) const { return !(*this == o); }
};

class OnboardingPreferenceStore : public arch::StateSource<OnboardingState> {
  public:
    // Persisted keys — verbatim from android (cross-client schema continuity).
    static constexpr const char* kKeyWelcomeCompleted = "onboarding_welcome_completed";
    static constexpr const char* kKeyDashboardHintDismissed = "onboarding_dashboard_hint_dismissed";

    // Production ctor: a QSettings under the app org. Test ctor: inject a store.
    OnboardingPreferenceStore();
    explicit OnboardingPreferenceStore(std::unique_ptr<QSettings> settings);
    ~OnboardingPreferenceStore() override;

    bool welcomeCompleted() const { return state().value().welcomeCompleted; }
    bool dashboardHintDismissed() const { return state().value().dashboardHintDismissed; }

    // Mark the welcome flow finished (pager Skip / wizard Finish). Persists +
    // republishes; idempotent (a second call does not re-emit).
    void markWelcomeCompleted();

    // Dismiss the dashboard "need a hand?" hint. Independent of welcomeCompleted.
    void dismissDashboardHint();

    // Clear both flags (re-show the pager on next launch).
    void resetWelcome();

  private:
    static OnboardingState readInitial(QSettings& settings);
    void persist(const OnboardingState& next);

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
