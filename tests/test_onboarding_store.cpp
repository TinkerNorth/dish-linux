// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Both flags default false so a first run shows the welcome pager. A relaunch is
// simulated by a second store over the same ini.

#include "source/store/OnboardingPreferenceStore.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

using dish::source::OnboardingPreferenceStore;
using dish::source::OnboardingState;
using dish::test::StateSourceProbe;

namespace {

std::unique_ptr<OnboardingPreferenceStore> makeStore(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("onboarding.ini"));
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    return std::make_unique<OnboardingPreferenceStore>(std::move(settings));
}

} // namespace

TEST_CASE("OnboardingPreferenceStore: fresh store has both flags false", "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);
    REQUIRE_FALSE(store->welcomeCompleted());
    REQUIRE_FALSE(store->dashboardHintDismissed());
}

TEST_CASE("OnboardingPreferenceStore: markWelcomeCompleted flips + persists",
          "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    {
        auto store = makeStore(dir);
        store->markWelcomeCompleted();
        REQUIRE(store->welcomeCompleted());
        REQUIRE_FALSE(store->dashboardHintDismissed());
    }
    {
        auto reloaded = makeStore(dir);
        REQUIRE(reloaded->welcomeCompleted());
        REQUIRE_FALSE(reloaded->dashboardHintDismissed());
    }
}

TEST_CASE("OnboardingPreferenceStore: dismissDashboardHint is independent of welcome",
          "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);
    store->dismissDashboardHint();
    REQUIRE(store->dashboardHintDismissed());
    REQUIRE_FALSE(store->welcomeCompleted());
}

TEST_CASE("OnboardingPreferenceStore: dashboardHintDismissed persists across relaunch",
          "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    {
        auto store = makeStore(dir);
        store->dismissDashboardHint();
    }
    {
        auto reloaded = makeStore(dir);
        REQUIRE(reloaded->dashboardHintDismissed());
    }
}

TEST_CASE("OnboardingPreferenceStore: resetWelcome clears both flags", "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);
    store->markWelcomeCompleted();
    store->dismissDashboardHint();
    REQUIRE(store->welcomeCompleted());
    REQUIRE(store->dashboardHintDismissed());

    store->resetWelcome();
    REQUIRE_FALSE(store->welcomeCompleted());
    REQUIRE_FALSE(store->dashboardHintDismissed());
}

TEST_CASE("OnboardingPreferenceStore: emission is distinct-until-changed", "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);

    StateSourceProbe<OnboardingState> probe(store->state());
    // The one emission is the current value replayed to the new subscriber.
    REQUIRE(probe.count() == 1);

    store->markWelcomeCompleted(); // real transition
    REQUIRE(probe.count() == 2);

    store->markWelcomeCompleted(); // redundant — no re-emit
    REQUIRE(probe.count() == 2);

    store->dismissDashboardHint(); // a different field changes
    REQUIRE(probe.count() == 3);

    store->dismissDashboardHint(); // redundant — no re-emit
    REQUIRE(probe.count() == 3);
}

TEST_CASE("OnboardingPreferenceStore: resetWelcome after no-op does not emit",
          "[onboarding][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);

    StateSourceProbe<OnboardingState> probe(store->state());
    REQUIRE(probe.count() == 1);
    // A fresh store is already {false,false}, so resetWelcome writes no change.
    store->resetWelcome();
    REQUIRE(probe.count() == 1);
}
