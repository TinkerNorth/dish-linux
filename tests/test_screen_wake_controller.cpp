// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// End-to-end over the whole wake split: streamingSlotCount -> WakeStateComposer
// -> WakeStateController -> inhibitor, wired the way AppModel wires it, with a
// fake inhibitor standing in for the session-bus one. The unit files pin each
// stage; this one pins that the stages are actually connected — a mode the user
// picked in Settings has to reach the OS, and a stream ending has to let go.

#include "architecture/Observable.h"
#include "composer/StreamingSlotCount.h"
#include "composer/WakeStateComposer.h"
#include "composer/WakeStateController.h"
#include "source/system/WakeInhibitor.h"

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QString>

using dish::arch::Observable;
using dish::composer::streamingSlotCount;
using dish::composer::WakeStateComposer;
using dish::composer::WakeStateController;
using dish::models::LinkState;
using dish::reducer::KeepAwakeMode;
using dish::reducer::KeepAwakePreferences;
using dish::reducer::KeepAwakeReach;
using dish::source::WakeInhibitor;

namespace {

// Absolute and idempotent, like the production inhibitor; `changes()` counts
// only the applies that actually moved the OS.
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

} // namespace

TEST_CASE("streamingSlotCount: zero when nothing is bound", "[wake]") {
    QHash<QString, QString> bindings;
    QHash<QString, LinkState> states;
    states.insert("c1", LinkState::Connected);
    REQUIRE(streamingSlotCount(bindings, states) == 0);
}

TEST_CASE("streamingSlotCount: ignores bindings to non-live LinkStates", "[wake]") {
    // Only Connected counts as streaming; every other state is a paired or
    // pending row whose session is not exchanging packets.
    QHash<QString, QString> bindings{
        {"slot-a", "conn-1"}, {"slot-b", "conn-2"}, {"slot-c", "conn-3"}};
    QHash<QString, LinkState> states{
        {"conn-1", LinkState::Saved},
        {"conn-2", LinkState::Connecting},
        {"conn-3", LinkState::Connected},
    };
    REQUIRE(streamingSlotCount(bindings, states) == 1);
}

TEST_CASE("streamingSlotCount: counts multiple connected slots", "[wake]") {
    QHash<QString, QString> bindings{{"a", "c1"}, {"b", "c2"}, {"c", "c3"}};
    QHash<QString, LinkState> states{
        {"c1", LinkState::Connected},
        {"c2", LinkState::Connected},
        {"c3", LinkState::Saved},
    };
    REQUIRE(streamingSlotCount(bindings, states) == 2);
}

TEST_CASE("streamingSlotCount: unknown connection counts as not-streaming", "[wake]") {
    QHash<QString, QString> bindings{{"a", "missing"}};
    QHash<QString, LinkState> states;
    REQUIRE(streamingSlotCount(bindings, states) == 0);
}

namespace {

KeepAwakePreferences prefs(KeepAwakeMode mode, bool display = false) {
    KeepAwakePreferences p;
    p.mode = mode;
    p.keepDisplayAwake = display;
    return p;
}

// Mirrors how AppModel wires the wake subsystem.
struct WakeHarness {
    Observable<int> count{0};
    Observable<bool> active{false};
    Observable<KeepAwakePreferences> preferences;
    WakeStateComposer composer{count, active, preferences};
    FakeInhibitor fake;
    WakeStateController controller{composer.state(), &fake, QStringLiteral("test reason")};

    explicit WakeHarness(KeepAwakePreferences initial = KeepAwakePreferences{},
                         bool controllerActive = true)
        : preferences(initial) {
        active.set(controllerActive);
        controller.start();
    }
};

} // namespace

TEST_CASE("wake: first stream holds the machine", "[wake]") {
    WakeHarness h;
    h.count.set(1);
    REQUIRE(h.fake.changes() == 1);
    REQUIRE(h.fake.held() == KeepAwakeReach::System);
    REQUIRE(h.fake.lastReason() == "test reason");
}

TEST_CASE("wake: 1 -> 2 slots does not move the OS", "[wake]") {
    WakeHarness h;
    h.count.set(1);
    h.count.set(2);
    REQUIRE(h.fake.changes() == 1);
}

TEST_CASE("wake: positive -> 0 releases", "[wake]") {
    WakeHarness h;
    h.count.set(2);
    h.count.set(0);
    REQUIRE(h.fake.changes() == 2);
    REQUIRE_FALSE(h.fake.isHeld());
}

TEST_CASE("wake: staying at 0 is idempotent", "[wake]") {
    WakeHarness h;
    h.count.set(0); // no change from the initial 0
    REQUIRE(h.fake.changes() == 0);
}

TEST_CASE("wake: re-acquires after a drop", "[wake]") {
    WakeHarness h;
    h.count.set(1);
    h.count.set(0);
    h.count.set(1);
    REQUIRE(h.fake.changes() == 3);
    REQUIRE(h.fake.isHeld());
}

TEST_CASE("wake: mode Off never holds, however many slots stream", "[wake]") {
    WakeHarness h(prefs(KeepAwakeMode::Off, true));
    h.count.set(3);
    REQUIRE(h.fake.changes() == 0);
    REQUIRE_FALSE(h.fake.isHeld());
}

TEST_CASE("wake: the timed mode holds only while the pad is being played", "[wake]") {
    WakeHarness h(prefs(KeepAwakeMode::WhileControllerActive), /*controllerActive=*/false);
    h.count.set(1);
    REQUIRE_FALSE(h.fake.isHeld()); // streaming, but nobody has touched it

    h.active.set(true);
    REQUIRE(h.fake.held() == KeepAwakeReach::System);

    // The idle window expiring must let the machine sleep again.
    h.active.set(false);
    REQUIRE_FALSE(h.fake.isHeld());
}

TEST_CASE("wake: WhileConnected ignores the idle window entirely", "[wake]") {
    WakeHarness h(prefs(KeepAwakeMode::WhileConnected), /*controllerActive=*/false);
    h.count.set(1);
    REQUIRE(h.fake.held() == KeepAwakeReach::System);

    h.active.set(true);
    h.active.set(false);
    REQUIRE(h.fake.held() == KeepAwakeReach::System);
    REQUIRE(h.fake.changes() == 1); // activity never moved the reach in this mode
}

TEST_CASE("wake: the display opt-in reaches the OS", "[wake]") {
    WakeHarness h(prefs(KeepAwakeMode::WhileConnected, true), /*controllerActive=*/false);
    h.count.set(1);
    REQUIRE(h.fake.held() == KeepAwakeReach::SystemAndDisplay);

    // Toggled off mid-stream, the panel is released without dropping the
    // system hold the stream still needs.
    h.preferences.set(prefs(KeepAwakeMode::WhileConnected, false));
    REQUIRE(h.fake.held() == KeepAwakeReach::System);
}

TEST_CASE("wake: switching modes mid-stream re-derives the hold", "[wake]") {
    WakeHarness h(prefs(KeepAwakeMode::WhileControllerActive), /*controllerActive=*/false);
    h.count.set(1);
    REQUIRE_FALSE(h.fake.isHeld());

    h.preferences.set(prefs(KeepAwakeMode::WhileConnected));
    REQUIRE(h.fake.held() == KeepAwakeReach::System);

    h.preferences.set(prefs(KeepAwakeMode::Off));
    REQUIRE_FALSE(h.fake.isHeld());
}

TEST_CASE("wake: stop releases the inhibitor (deliberate teardown)", "[wake]") {
    WakeHarness h;
    h.count.set(3);
    h.controller.stop();
    REQUIRE(h.fake.held() == KeepAwakeReach::None);
}

TEST_CASE("wake: tolerates a null inhibitor", "[wake]") {
    // A headless or stripped-down build can pass nullptr; the controller must
    // still bookkeep without crashing.
    Observable<int> count{0};
    Observable<bool> active{true};
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer{count, active, preferences};
    WakeStateController controller{composer.state(), nullptr};
    controller.start();
    count.set(1);
    count.set(0);
    controller.stop();
    REQUIRE_FALSE(controller.isInhibiting());
}
