// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/CrashReportingBackend.h"
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
