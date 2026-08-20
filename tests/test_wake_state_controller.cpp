// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Driven through a fake inhibitor, so the real session-bus inhibit is
// never flipped for the test process.

#include "Util/DisplaySleepInhibitor.h"
#include "architecture/Observable.h"
#include "composer/WakeStateComposer.h"
#include "composer/WakeStateController.h"

#include "ControllerProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::arch::Observable;
using dish::composer::WakeState;
using dish::composer::WakeStateComposer;
using dish::composer::WakeStateController;
using dish::test::ControllerProbe;
using dish::util::DisplaySleepInhibitor;

namespace {

// Idempotent acquire/release, like the production
// FreedesktopScreenSaverInhibitor.
class FakeInhibitor : public DisplaySleepInhibitor {
  public:
    void acquire(const QString& reason) override {
        if (!held_) {
            ++acquires_;
            held_ = true;
            lastReason_ = reason;
        }
    }
    void release() override {
        if (held_) {
            ++releases_;
            held_ = false;
        }
    }
    bool isHeld() const override { return held_; }

    int acquires() const { return acquires_; }
    int releases() const { return releases_; }
    QString lastReason() const { return lastReason_; }

  private:
    int acquires_ = 0;
    int releases_ = 0;
    bool held_ = false;
    QString lastReason_;
};

WakeState inhibit(int count) { return WakeState{true, count}; }
WakeState idle() { return WakeState{false, 0}; }

} // namespace

TEST_CASE("WakeStateController: start while already inhibiting acquires at once", "[wake]") {
    Observable<WakeState> ws(inhibit(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake, QStringLiteral("test reason"));
    REQUIRE(fake.acquires() == 0); // not started yet
    c.start();
    REQUIRE(fake.acquires() == 1);
    REQUIRE(fake.isHeld());
    REQUIRE(fake.lastReason() == QStringLiteral("test reason"));
}

TEST_CASE("WakeStateController: start while idle does not acquire", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.acquires() == 0);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: a second start does not re-acquire", "[wake]") {
    Observable<WakeState> ws(inhibit(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.start();
    REQUIRE(fake.acquires() == 1);
}

TEST_CASE("WakeStateController: idle then a stream acquires", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(inhibit(1));
    REQUIRE(fake.acquires() == 1);
    REQUIRE(fake.isHeld());
}

TEST_CASE("WakeStateController: streaming then idle releases", "[wake]") {
    Observable<WakeState> ws(inhibit(2));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.acquires() == 1);
    ws.set(idle());
    REQUIRE(fake.releases() == 1);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: a count change while inhibiting does not re-acquire", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(inhibit(1));
    ws.set(inhibit(2)); // still inhibiting; idempotent acquire is a no-op
    ws.set(inhibit(3));
    REQUIRE(fake.acquires() == 1);
    REQUIRE(fake.releases() == 0);
}

TEST_CASE("WakeStateController: re-acquires after a drop to idle", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    ws.set(inhibit(1));
    ws.set(idle());
    ws.set(inhibit(1));
    REQUIRE(fake.acquires() == 2);
    REQUIRE(fake.releases() == 1);
    REQUIRE(fake.isHeld());
}

TEST_CASE("WakeStateController: stop releases the held inhibitor", "[wake]") {
    Observable<WakeState> ws(inhibit(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE(fake.isHeld());
    c.stop();
    REQUIRE(fake.releases() == 1);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: stop while idle is a quiet no-op on the inhibitor", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE(fake.acquires() == 0);
    REQUIRE(fake.releases() == 0);
}

TEST_CASE("WakeStateController: emissions after stop do not acquire", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    ws.set(inhibit(1));
    REQUIRE(fake.acquires() == 0);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: restart after stop re-applies the current value", "[wake]") {
    Observable<WakeState> ws(inhibit(1));
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    REQUIRE_FALSE(fake.isHeld());
    c.start(); // onStarting re-arms and re-applies the current value
    REQUIRE(fake.isHeld());
    REQUIRE(fake.acquires() == 2);
}

TEST_CASE("WakeStateController: after restart, transitions actuate again", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    c.stop();
    c.start();
    ws.set(inhibit(1));
    REQUIRE(fake.isHeld());
    ws.set(idle());
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("WakeStateController: isInhibiting reflects the inhibitor state", "[wake]") {
    Observable<WakeState> ws(idle());
    FakeInhibitor fake;
    WakeStateController c(ws, &fake);
    c.start();
    REQUIRE_FALSE(c.isInhibiting());
    ws.set(inhibit(1));
    REQUIRE(c.isInhibiting());
    ws.set(idle());
    REQUIRE_FALSE(c.isInhibiting());
}

TEST_CASE("WakeStateController: ControllerProbe start/stop drives the effect", "[wake]") {
    Observable<WakeState> ws(inhibit(1));
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
    ws.set(inhibit(1));
    ws.set(idle());
    c.stop();
    REQUIRE_FALSE(c.isInhibiting()); // null inhibitor is never "held"
}

TEST_CASE("WakeStateController: end-to-end via the composer acquires on first stream", "[wake]") {
    Observable<int> count(0);
    Observable<int> keepOn(0);
    WakeStateComposer composer(count, keepOn);
    FakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    REQUIRE_FALSE(fake.isHeld());

    count.set(1);
    REQUIRE(fake.isHeld());
    REQUIRE(fake.acquires() == 1);

    count.set(2);
    REQUIRE(fake.acquires() == 1);

    count.set(0);
    REQUIRE_FALSE(fake.isHeld());
    REQUIRE(fake.releases() == 1);
}

TEST_CASE("WakeStateController: end-to-end keep-screen-on override holds at zero count", "[wake]") {
    Observable<int> count(0);
    Observable<int> keepOn(0);
    WakeStateComposer composer(count, keepOn);
    FakeInhibitor fake;
    WakeStateController c(composer.state(), &fake);
    c.start();
    keepOn.set(1);
    REQUIRE(fake.isHeld());
    keepOn.set(0);
    REQUIRE_FALSE(fake.isHeld());
}
