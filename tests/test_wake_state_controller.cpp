// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The effect half of the wake subsystem. apply() is absolute and idempotent, so
// what is pinned here is that the controller only ever moves the OS when the
// reach really changed, and that no path — stop, restart, a null inhibitor —
// can strand a hold on the machine. Driven through a fake inhibitor, so the
// real session-bus inhibit is never flipped for the test process.

#include "architecture/Observable.h"
#include "composer/WakeStateComposer.h"
#include "composer/WakeStateController.h"
#include "source/system/WakeInhibitor.h"

#include "ControllerProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::arch::Observable;
using dish::composer::WakeState;
using dish::composer::WakeStateComposer;
using dish::composer::WakeStateController;
using dish::reducer::KeepAwakeMode;
using dish::reducer::KeepAwakePreferences;
using dish::reducer::KeepAwakeReach;
using dish::source::WakeInhibitor;
using dish::test::ControllerProbe;

namespace {

// Idempotent and absolute, like the production FreedesktopWakeInhibitor.
// `changes()` counts only the applies that actually moved the held reach, so
// the "does not re-apply" assertions still mean something now that every
// emission reaches apply().
class FakeInhibitor : public WakeInhibitor {
  public:
    void apply(KeepAwakeReach reach, const QString& reason) override {
        ++applies_;
        lastReason_ = reason;
        if (reach == held_) { return; }
        held_ = reach;
        ++changes_;
    }
    KeepAwakeReach held() const override { return held_; }

    int applies() const { return applies_; }
    int changes() const { return changes_; }
    QString lastReason() const { return lastReason_; }
    bool isHeld() const { return held_ != KeepAwakeReach::None; }

  private:
    int applies_ = 0;
    int changes_ = 0;
    KeepAwakeReach held_ = KeepAwakeReach::None;
    QString lastReason_;
};

WakeState hold(int count, KeepAwakeReach reach = KeepAwakeReach::System) {
    return WakeState{reach, count};
}
WakeState idle() { return WakeState{KeepAwakeReach::None, 0}; }

KeepAwakePreferences prefs(KeepAwakeMode mode, bool display = false) {
    KeepAwakePreferences p;
    p.mode = mode;
    p.keepDisplayAwake = display;
    return p;
}

} // namespace

TEST_CASE("WakeStateController: start while already holding applies at once", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake, QStringLiteral("test reason"));
    REQUIRE(fake.applies() == 0); // not started yet
    c.start();
    REQUIRE(fake.changes() == 1);
    REQUIRE(fake.held() == KeepAwakeReach::System);
    REQUIRE(fake.lastReason() == QStringLiteral("test reason"));
}

TEST_CASE("WakeStateController: start while idle does not hold", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.changes() == 0);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: a second start does not re-apply", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.start();
    REQUIRE(fake.changes() == 1);
}

TEST_CASE("WakeStateController: idle then a stream acquires", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(hold(1));
    REQUIRE(fake.changes() == 1);
    REQUIRE(fake.held() == KeepAwakeReach::System);
}

TEST_CASE("WakeStateController: streaming then idle releases", "[wake]") {
    Observable<WakeState> ws(hold(2));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.changes() == 1);
    ws.set(idle());
    REQUIRE(fake.changes() == 2);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: a count change while holding does not move the reach", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(hold(1));
    ws.set(hold(2)); // still the same reach; the idempotent apply is a no-op
    ws.set(hold(3));
    REQUIRE(fake.applies() == 4); // every emission still reaches the inhibitor
    REQUIRE(fake.changes() == 1); // but only the first moved the OS
}

TEST_CASE("WakeStateController: widening to the display is a real move", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.held() == KeepAwakeReach::System);

    ws.set(hold(1, KeepAwakeReach::SystemAndDisplay));
    REQUIRE(fake.changes() == 2);
    REQUIRE(fake.held() == KeepAwakeReach::SystemAndDisplay);

    ws.set(hold(1, KeepAwakeReach::System)); // narrowing back is a move too
    REQUIRE(fake.changes() == 3);
    REQUIRE(fake.held() == KeepAwakeReach::System);
}

TEST_CASE("WakeStateController: re-acquires after a drop to idle", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(hold(1));
    ws.set(idle());
    ws.set(hold(1));
    REQUIRE(fake.changes() == 3);
    REQUIRE(fake.held() == KeepAwakeReach::System);
}

TEST_CASE("WakeStateController: stop releases the held inhibitor", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.isHeld());
    c.stop();
    REQUIRE(fake.held() == KeepAwakeReach::None);
}

TEST_CASE("WakeStateController: stop releases a display hold too", "[wake]") {
    Observable<WakeState> ws(hold(1, KeepAwakeReach::SystemAndDisplay));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE(fake.held() == KeepAwakeReach::None);
}

TEST_CASE("WakeStateController: stop while idle is a quiet no-op on the inhibitor", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE(fake.changes() == 0);
}

TEST_CASE("WakeStateController: emissions after stop do not acquire", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    ws.set(hold(1));
    REQUIRE(fake.changes() == 0);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: restart after stop re-applies the current value", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE_FALSE(fake.isHeld());
    c.start(); // onStarting re-arms and re-applies the current value
    REQUIRE(fake.isHeld());
    REQUIRE(fake.changes() == 3); // acquire, stop's release, re-acquire
}

TEST_CASE("WakeStateController: after restart, transitions actuate again", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    c.start();
    ws.set(hold(1));
    REQUIRE(fake.isHeld());
    ws.set(idle());
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: held and isInhibiting mirror the inhibitor", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(c.held() == KeepAwakeReach::None);
    REQUIRE_FALSE(c.isInhibiting());

    ws.set(hold(1));
    REQUIRE(c.held() == KeepAwakeReach::System);
    REQUIRE(c.isInhibiting());

    ws.set(hold(1, KeepAwakeReach::SystemAndDisplay));
    REQUIRE(c.held() == KeepAwakeReach::SystemAndDisplay);
    REQUIRE(c.isInhibiting());

    ws.set(idle());
    REQUIRE(c.held() == KeepAwakeReach::None);
    REQUIRE_FALSE(c.isInhibiting());
}

TEST_CASE("WakeStateController: ControllerProbe start/stop drives the effect", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    ControllerProbe<WakeStateController> probe(c);
    probe.start();
    REQUIRE(fake.isHeld());
    probe.stop();
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: tolerates a null inhibitor", "[wake]") {
    Observable<WakeState> ws(idle());
    WakeStateController c(ws, nullptr);
    c.start();
    ws.set(hold(1));
    ws.set(idle());
    c.stop();
    // A null inhibitor is never "held", so a headless build reports honestly.
    REQUIRE(c.held() == KeepAwakeReach::None);
    REQUIRE_FALSE(c.isInhibiting());
}

TEST_CASE("WakeStateController: end-to-end via the composer holds on first stream", "[wake]") {
    Observable<int> count(0);
    Observable<bool> active(true);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    FakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE_FALSE(fake.isHeld());

    count.set(1);
    REQUIRE(fake.held() == KeepAwakeReach::System);
    REQUIRE(fake.changes() == 1);

    count.set(2);
    REQUIRE(fake.changes() == 1);

    count.set(0);
    REQUIRE_FALSE(fake.isHeld());
    REQUIRE(fake.changes() == 2);
}

TEST_CASE("WakeStateController: end-to-end an idle pad drops the hold in the timed mode",
          "[wake]") {
    Observable<int> count(1);
    Observable<bool> active(true);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    FakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE(fake.isHeld());

    active.set(false);
    REQUIRE_FALSE(fake.isHeld());
    active.set(true);
    REQUIRE(fake.isHeld());
}

TEST_CASE("WakeStateController: end-to-end the display opt-in widens a live hold", "[wake]") {
    Observable<int> count(1);
    Observable<bool> active(false);
    Observable<KeepAwakePreferences> preferences{prefs(KeepAwakeMode::WhileConnected)};
    WakeStateComposer composer(count, active, preferences);
    FakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE(fake.held() == KeepAwakeReach::System);

    preferences.set(prefs(KeepAwakeMode::WhileConnected, true));
    REQUIRE(fake.held() == KeepAwakeReach::SystemAndDisplay);

    preferences.set(prefs(KeepAwakeMode::Off, true));
    REQUIRE(fake.held() == KeepAwakeReach::None);
}
