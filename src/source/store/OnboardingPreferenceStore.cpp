// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/OnboardingPreferenceStore.h"

#include <QSettings>

namespace dish::source {

OnboardingState OnboardingPreferenceStore::readInitial(QSettings& settings) {
    OnboardingState s;
    s.welcomeCompleted = settings.value(QLatin1String(kKeyWelcomeCompleted), false).toBool();
    s.dashboardHintDismissed =
        settings.value(QLatin1String(kKeyDashboardHintDismissed), false).toBool();
    return s;
}

OnboardingPreferenceStore::OnboardingPreferenceStore()
    : OnboardingPreferenceStore(
          std::make_unique<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {}

OnboardingPreferenceStore::OnboardingPreferenceStore(std::unique_ptr<QSettings> settings)
    : arch::StateSource<OnboardingState>(readInitial(*settings)), settings_(std::move(settings)) {}

OnboardingPreferenceStore::~OnboardingPreferenceStore() = default;

void OnboardingPreferenceStore::persist(const OnboardingState& next) {
    settings_->setValue(QLatin1String(kKeyWelcomeCompleted), next.welcomeCompleted);
    settings_->setValue(QLatin1String(kKeyDashboardHintDismissed), next.dashboardHintDismissed);
}

void OnboardingPreferenceStore::markWelcomeCompleted() {
    // Persist the whole next state, then republish (distinct-until-changed in the
    // Observable suppresses a redundant flip).
    OnboardingState next = state().value();
    next.welcomeCompleted = true;
    persist(next);
    setState(next);
}

void OnboardingPreferenceStore::dismissDashboardHint() {
    OnboardingState next = state().value();
    next.dashboardHintDismissed = true;
    persist(next);
    setState(next);
}

void OnboardingPreferenceStore::resetWelcome() {
    const OnboardingState cleared{false, false};
    persist(cleared);
    setState(cleared);
}

} // namespace dish::source
