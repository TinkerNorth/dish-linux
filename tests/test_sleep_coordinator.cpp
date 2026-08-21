// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Driven through a fake monitor and the effect-sink constructor, so the
// suspend edge is exercised without a system bus, a delay lock or a live
// satellite. The sink records rather than acts: what is pinned here is which
// effects come out, and in what order.

#include "composer/SleepCoordinator.h"
#include "core/reducer/SleepCycle.h"
#include "source/system/SleepMonitor.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::composer::SleepCoordinator;
using dish::reducer::SleepEffect;
using dish::reducer::SleepPhase;

namespace {

// start()/stop() only record: the coordinator does not own the monitor's
// lifecycle, and a fake that acted on them would hide that.
class FakeSleepMonitor : public dish::source::SleepMonitor {
  public:
    void start() override { ++starts_; }
    void stop() override { ++stops_; }

    // The edge logind would deliver.
    void emitPreparingForSleep(bool starting) { emit preparingForSleep(starting); }

    int starts() const { return starts_; }
    int stops() const { return stops_; }

  private:
    int starts_ = 0;
    int stops_ = 0;
};

} // namespace

TEST_CASE("SleepCoordinator: a suspend tears the sessions down exactly once", "[sleep]") {
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });
    REQUIRE(effects.empty());

    monitor.emitPreparingForSleep(true);
    REQUIRE(effects.size() == 1);
    REQUIRE(effects.at(0) == SleepEffect::TearDown);
    REQUIRE(c.phase() == SleepPhase::Suspending);
}

TEST_CASE("SleepCoordinator: a repeated suspend emits nothing more", "[sleep]") {
    // logind repeats PrepareForSleep(true) when a suspend escalates to
    // hibernate; a second teardown would fire against sessions already gone.
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });

    monitor.emitPreparingForSleep(true);
    monitor.emitPreparingForSleep(true);
    monitor.emitPreparingForSleep(true);
    REQUIRE(effects.size() == 1);
    REQUIRE(c.phase() == SleepPhase::Suspending);
}

TEST_CASE("SleepCoordinator: a resume reconnects exactly once", "[sleep]") {
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });

    monitor.emitPreparingForSleep(true);
    monitor.emitPreparingForSleep(false);
    REQUIRE(effects.size() == 2);
    REQUIRE(effects.at(1) == SleepEffect::Reconnect);
    REQUIRE(c.phase() == SleepPhase::Awake);

    monitor.emitPreparingForSleep(false);
    REQUIRE(effects.size() == 2);
}

TEST_CASE("SleepCoordinator: a resume with no prior suspend emits nothing", "[sleep]") {
    // Dish can start after the suspend edge, or miss it on a bus reconnect.
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });

    monitor.emitPreparingForSleep(false);
    REQUIRE(effects.empty());
    REQUIRE(c.phase() == SleepPhase::Awake);
}

TEST_CASE("SleepCoordinator: a full cycle emits teardown and reconnect in order", "[sleep]") {
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });

    monitor.emitPreparingForSleep(true);
    monitor.emitPreparingForSleep(false);
    monitor.emitPreparingForSleep(true);
    monitor.emitPreparingForSleep(false);

    REQUIRE(effects.size() == 4);
    REQUIRE(effects.at(0) == SleepEffect::TearDown);
    REQUIRE(effects.at(1) == SleepEffect::Reconnect);
    REQUIRE(effects.at(2) == SleepEffect::TearDown);
    REQUIRE(effects.at(3) == SleepEffect::Reconnect);
    REQUIRE(c.phase() == SleepPhase::Awake);
}

TEST_CASE("SleepCoordinator: phase tracks the edge throughout", "[sleep]") {
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });
    REQUIRE(c.phase() == SleepPhase::Awake);

    monitor.emitPreparingForSleep(true);
    REQUIRE(c.phase() == SleepPhase::Suspending);
    monitor.emitPreparingForSleep(true);
    REQUIRE(c.phase() == SleepPhase::Suspending);
    monitor.emitPreparingForSleep(false);
    REQUIRE(c.phase() == SleepPhase::Awake);
    monitor.emitPreparingForSleep(false);
    REQUIRE(c.phase() == SleepPhase::Awake);
}

TEST_CASE("SleepCoordinator: the monitor's lifecycle is left to the shell", "[sleep]") {
    // The monitor is borrowed, not owned: whoever built it starts it.
    FakeSleepMonitor monitor;
    std::vector<SleepEffect> effects;
    SleepCoordinator c(&monitor, [&effects](SleepEffect e) { effects.push_back(e); });
    REQUIRE(monitor.starts() == 0);
    REQUIRE(monitor.stops() == 0);
    REQUIRE(c.phase() == SleepPhase::Awake);
}

TEST_CASE("SleepCoordinator: tolerates a null monitor", "[sleep]") {
    // A build without logind has no edge to subscribe to; construction still
    // has to succeed, and the phase stays awake forever.
    std::vector<SleepEffect> effects;
    SleepCoordinator c(nullptr, [&effects](SleepEffect e) { effects.push_back(e); });
    REQUIRE(effects.empty());
    REQUIRE(c.phase() == SleepPhase::Awake);
}
