// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/CrashReportingBackend.h"

#include <string>
#include "composer/CrashReportingController.h"
#include "source/store/CrashReportingStore.h"

#include "ControllerProbe.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <vector>

using dish::composer::CrashReportingBackend;
using dish::composer::CrashReportingController;
using dish::source::CrashReportingStore;
using dish::test::ControllerProbe;
using dish::test::StateSourceProbe;

namespace {

std::unique_ptr<CrashReportingStore> makeStore(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("crash.ini"));
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    return std::make_unique<CrashReportingStore>(std::move(settings));
}

class FakeCrashReportingBackend : public CrashReportingBackend {
  public:
    void setEnabled(bool enabled) override { flips_.push_back(enabled); }
    const std::vector<bool>& flips() const { return flips_; }

  private:
    std::vector<bool> flips_;
};

} // namespace

TEST_CASE("CrashReportingStore defaults to ENABLED (opt-out, matches android)", "[crash][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);
    REQUIRE(store->enabled());
    REQUIRE(CrashReportingStore::kDefaultEnabled == true);
}

TEST_CASE("CrashReportingStore: opt out then back in persists across a relaunch",
          "[crash][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    {
        auto store = makeStore(dir);
        store->setEnabled(false);
    }
    {
        auto reloaded = makeStore(dir);
        REQUIRE_FALSE(reloaded->enabled());
        reloaded->setEnabled(true);
    }
    {
        auto reloaded = makeStore(dir);
        REQUIRE(reloaded->enabled());
    }
}

TEST_CASE("CrashReportingStore emits distinct-until-changed", "[crash][store]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir); // default true

    StateSourceProbe<bool> probe(store->state());
    REQUIRE(probe.count() == 1); // replayed current (true)

    store->setEnabled(true); // already true — no-op
    REQUIRE(probe.count() == 1);

    store->setEnabled(false);
    REQUIRE(probe.count() == 2);

    store->setEnabled(false); // redundant — no re-emit
    REQUIRE(probe.count() == 2);
}

TEST_CASE("CrashReportingController forwards each flip to the backend", "[crash][controller]") {
    dish::arch::Observable<bool> enabled{true};
    FakeCrashReportingBackend backend;
    CrashReportingController controller(enabled, &backend);

    ControllerProbe<CrashReportingController> probe(controller);
    probe.start(); // start() applies the current value immediately
    REQUIRE(backend.flips() == std::vector<bool>{true});

    enabled.set(false);
    enabled.set(true);
    REQUIRE(backend.flips() == std::vector<bool>{true, false, true});
}

TEST_CASE("CrashReportingController SURVIVES stop() -- the opt-in keeps propagating",
          "[crash][controller]") {
    dish::arch::Observable<bool> enabled{true};
    FakeCrashReportingBackend backend;
    CrashReportingController controller(enabled, &backend);

    ControllerProbe<CrashReportingController> probe(controller);
    probe.start();
    REQUIRE(backend.flips().size() == 1); // the initial true

    // stop() is a deliberate no-op: the opt-in is process-scoped, so unlike the
    // kernel default this controller does not cancel its subscription.
    probe.stop();
    enabled.set(false);
    REQUIRE(backend.flips() == std::vector<bool>{true, false});
    enabled.set(true);
    REQUIRE(backend.flips() == std::vector<bool>{true, false, true});
}

TEST_CASE("CrashReportingController: start() is idempotent (applies once)", "[crash][controller]") {
    dish::arch::Observable<bool> enabled{true};
    FakeCrashReportingBackend backend;
    CrashReportingController controller(enabled, &backend);

    controller.start();
    controller.start(); // kernel idempotency guard
    REQUIRE(backend.flips() == std::vector<bool>{true});

    // One subscription, not two.
    enabled.set(false);
    REQUIRE(backend.flips() == std::vector<bool>{true, false});
}

// ---------------------------------------------------------------------------
// Sentry arming policy. Mirrors dish-windows/tests/test_crash_reporting.cpp so
// the two clients cannot drift on what the switch means.
//
// This is the whole privacy property, so it is tested as a pure function rather
// than inferred from an integration run. Two gates have to hold: the build must
// carry a DSN, and the user must not have opted out. Neither alone is enough.

TEST_CASE("shouldArmSentry: a build with no DSN cannot report, even when enabled",
          "[crash][sentry]") {
    // Every local build, every PR build and every build from a fork, because
    // only release.yml injects the secret. On this platform such a build does
    // not even fetch the SDK.
    CHECK_FALSE(dish::composer::shouldArmSentry("", nullptr, true));
    CHECK_FALSE(dish::composer::shouldArmSentry(nullptr, nullptr, true));
    CHECK_FALSE(dish::composer::shouldArmSentry("", "", true));
}

TEST_CASE("shouldArmSentry: opting out beats any DSN", "[crash][sentry]") {
    const char* dsn = "https://key@o1.ingest.de.sentry.io/2";
    CHECK_FALSE(dish::composer::shouldArmSentry(dsn, nullptr, false));
    // Including the developer escape hatch: aiming a build at your own project
    // is not a reason to transmit from a machine that is not yours.
    CHECK_FALSE(dish::composer::shouldArmSentry("", dsn, false));
    CHECK_FALSE(dish::composer::shouldArmSentry(dsn, dsn, false));
}

TEST_CASE("shouldArmSentry: a compiled DSN plus consent arms", "[crash][sentry]") {
    CHECK(dish::composer::shouldArmSentry("https://key@o1.ingest.de.sentry.io/2", nullptr, true));
}

TEST_CASE("shouldArmSentry: $SENTRY_DSN substitutes for a compiled DSN", "[crash][sentry]") {
    const char* dsn = "https://key@o1.ingest.de.sentry.io/2";
    CHECK(dish::composer::shouldArmSentry("", dsn, true));
    CHECK(dish::composer::shouldArmSentry(nullptr, dsn, true));
}

TEST_CASE("the test binary itself carries no DSN and reports as development", "[crash][sentry]") {
    // If this ever fails, a build has been configured in a way that would let
    // the test suite transmit, which no test run should be able to do. It also
    // pins the environment derivation: no DSN means development, always.
    CHECK(std::string(dish::composer::compiledSentryDsn()).empty());
    CHECK(std::string(dish::composer::sentryEnvironment()) == "development");
    // And with no DSN the SDK is never fetched, so it cannot be linked either.
    CHECK_FALSE(dish::composer::sentrySdkAvailable());
}

TEST_CASE("SentryCrashReportingBackend stays inert without a DSN", "[crash][sentry]") {
    // The switch is opt-out, so this backend is asked to arm on nearly every
    // launch. On a build with no DSN that must be a quiet no-op, not a crash,
    // and it must never claim to be active.
    dish::composer::SentryCrashReportingBackend backend(std::string{});
    CHECK_FALSE(backend.active());
    backend.setEnabled(true);
    CHECK_FALSE(backend.active());
    backend.setEnabled(false);
    CHECK_FALSE(backend.active());
}
