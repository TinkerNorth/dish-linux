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
#include "FakeWakeInhibitor.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::arch::Observable;
using dish::composer::WakeState;
using dish::composer::WakeStateComposer;
using dish::composer::WakeStateController;
using dish::reducer::KeepAwakeHold;
using dish::reducer::KeepAwakeMode;
using dish::reducer::KeepAwakePreferences;
using dish::reducer::KeepAwakeReach;
using dish::test::ControllerProbe;
using dish::test::FakeWakeInhibitor;

namespace {

WakeState hold(int count, KeepAwakeReach reach = KeepAwakeReach::System) {
    return WakeState{reach, count, false};
}
WakeState holdWithLid(int count) { return WakeState{KeepAwakeReach::System, count, true}; }
WakeState idle() { return WakeState{KeepAwakeReach::None, 0, false}; }

KeepAwakePreferences prefs(KeepAwakeMode mode, bool display = false, bool lid = false) {
    KeepAwakePreferences p;
    p.mode = mode;
    p.keepDisplayAwake = display;
    p.keepAwakeWithLidClosed = lid;
    return p;
}

} // namespace

TEST_CASE("WakeStateController: start while already holding applies at once", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake, QStringLiteral("test reason"));
    REQUIRE(fake.applies() == 0); // not started yet
    c.start();
    REQUIRE(fake.changes() == 1);
    REQUIRE(fake.held().reach == KeepAwakeReach::System);
    REQUIRE(fake.lastReason() == QStringLiteral("test reason"));
}

TEST_CASE("WakeStateController: start while idle does not hold", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.changes() == 0);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: a second start does not re-apply", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.start();
    REQUIRE(fake.changes() == 1);
}

TEST_CASE("WakeStateController: idle then a stream acquires", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(hold(1));
    REQUIRE(fake.changes() == 1);
    REQUIRE(fake.held().reach == KeepAwakeReach::System);
}

TEST_CASE("WakeStateController: streaming then idle releases", "[wake]") {
    Observable<WakeState> ws(hold(2));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.changes() == 1);
    ws.set(idle());
    REQUIRE(fake.changes() == 2);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: a count change while holding does not move the reach", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeWakeInhibitor fake;
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
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.held().reach == KeepAwakeReach::System);

    ws.set(hold(1, KeepAwakeReach::SystemAndDisplay));
    REQUIRE(fake.changes() == 2);
    REQUIRE(fake.held().reach == KeepAwakeReach::SystemAndDisplay);

    ws.set(hold(1, KeepAwakeReach::System)); // narrowing back is a move too
    REQUIRE(fake.changes() == 3);
    REQUIRE(fake.held().reach == KeepAwakeReach::System);
}

TEST_CASE("WakeStateController: re-acquires after a drop to idle", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(hold(1));
    ws.set(idle());
    ws.set(hold(1));
    REQUIRE(fake.changes() == 3);
    REQUIRE(fake.held().reach == KeepAwakeReach::System);
}

TEST_CASE("WakeStateController: stop releases the held inhibitor", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.isHeld());
    c.stop();
    REQUIRE(fake.held().reach == KeepAwakeReach::None);
}

TEST_CASE("WakeStateController: stop releases a display hold too", "[wake]") {
    Observable<WakeState> ws(hold(1, KeepAwakeReach::SystemAndDisplay));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE(fake.held().reach == KeepAwakeReach::None);
}

TEST_CASE("WakeStateController: stop while idle is a quiet no-op on the inhibitor", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE(fake.changes() == 0);
}

TEST_CASE("WakeStateController: emissions after stop do not acquire", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    ws.set(hold(1));
    REQUIRE(fake.changes() == 0);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: restart after stop re-applies the current value", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeWakeInhibitor fake;
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
    FakeWakeInhibitor fake;
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
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(c.held().reach == KeepAwakeReach::None);
    REQUIRE_FALSE(c.isInhibiting());

    ws.set(hold(1));
    REQUIRE(c.held().reach == KeepAwakeReach::System);
    REQUIRE(c.isInhibiting());

    ws.set(hold(1, KeepAwakeReach::SystemAndDisplay));
    REQUIRE(c.held().reach == KeepAwakeReach::SystemAndDisplay);
    REQUIRE(c.isInhibiting());

    ws.set(idle());
    REQUIRE(c.held().reach == KeepAwakeReach::None);
    REQUIRE_FALSE(c.isInhibiting());
}

TEST_CASE("WakeStateController: the lid rides with the reach to the inhibitor", "[wake][lid]") {
    Observable<WakeState> ws(holdWithLid(1));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.held() == KeepAwakeHold{KeepAwakeReach::System, true});
    REQUIRE(c.held() == KeepAwakeHold{KeepAwakeReach::System, true});
}

TEST_CASE("WakeStateController: dropping the lid keeps the machine held", "[wake][lid]") {
    Observable<WakeState> ws(holdWithLid(1));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(hold(1));
    REQUIRE(fake.changes() == 2);
    REQUIRE(fake.held() == KeepAwakeHold{KeepAwakeReach::System, false});
}

TEST_CASE("WakeStateController: stop releases the lid with the machine", "[wake][lid]") {
    Observable<WakeState> ws(holdWithLid(1));
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE(fake.held() == KeepAwakeHold{});
    REQUIRE_FALSE(c.isInhibiting());
}

TEST_CASE("WakeStateController: a refused lid lock reads as not held", "[wake][lid]") {
    Observable<WakeState> ws(holdWithLid(1));
    FakeWakeInhibitor fake;
    fake.refuseLid = true;
    WakeStateController c(ws, &fake);
    c.start();
    // The machine is still held, so the controller is inhibiting; the lid is
    // reported as the inhibitor achieved it, not as the state asked for it.
    REQUIRE(c.held() == KeepAwakeHold{KeepAwakeReach::System, false});
    REQUIRE(c.isInhibiting());
}

TEST_CASE("WakeStateController: a lid held on its own still counts as inhibiting", "[wake][lid]") {
    // What the real inhibitor reports when logind refuses "idle" but grants
    // "handle-lid-switch": closing the lid is still held off.
    Observable<WakeState> ws(WakeState{KeepAwakeReach::None, 1, true});
    FakeWakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(c.held() == KeepAwakeHold{KeepAwakeReach::None, true});
    REQUIRE(c.isInhibiting());
}

TEST_CASE("WakeStateController: ControllerProbe start/stop drives the effect", "[wake]") {
    Observable<WakeState> ws(hold(1));
    FakeWakeInhibitor fake;
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
    REQUIRE(c.held().reach == KeepAwakeReach::None);
    REQUIRE_FALSE(c.isInhibiting());
}

TEST_CASE("WakeStateController: end-to-end via the composer holds on first stream", "[wake]") {
    Observable<int> count(0);
    Observable<bool> active(true);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    FakeWakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE_FALSE(fake.isHeld());

    count.set(1);
    REQUIRE(fake.held().reach == KeepAwakeReach::System);
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
    FakeWakeInhibitor fake;
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
    FakeWakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE(fake.held().reach == KeepAwakeReach::System);

    preferences.set(prefs(KeepAwakeMode::WhileConnected, true));
    REQUIRE(fake.held().reach == KeepAwakeReach::SystemAndDisplay);

    preferences.set(prefs(KeepAwakeMode::Off, true));
    REQUIRE(fake.held().reach == KeepAwakeReach::None);
}

TEST_CASE("WakeStateController: end-to-end the lid opt-in widens a live hold", "[wake][lid]") {
    Observable<int> count(1);
    Observable<bool> active(false);
    Observable<KeepAwakePreferences> preferences{prefs(KeepAwakeMode::WhileConnected)};
    WakeStateComposer composer(count, active, preferences);
    FakeWakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE(fake.held() == KeepAwakeHold{KeepAwakeReach::System, false});

    preferences.set(prefs(KeepAwakeMode::WhileConnected, false, true));
    REQUIRE(fake.held() == KeepAwakeHold{KeepAwakeReach::System, true});

    count.set(0);
    REQUIRE(fake.held() == KeepAwakeHold{});
}
