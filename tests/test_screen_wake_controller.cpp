// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// End-to-end over the wake split: streamingSlotCount -> WakeStateComposer ->
// WakeStateController -> inhibitor, with a fake inhibitor standing in for the
// session-bus one.

#include "Util/DisplaySleepInhibitor.h"
#include "architecture/Observable.h"
#include "composer/StreamingSlotCount.h"
#include "composer/WakeStateComposer.h"
#include "composer/WakeStateController.h"

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QString>

using dish::arch::Observable;
using dish::composer::streamingSlotCount;
using dish::composer::WakeState;
using dish::composer::WakeStateComposer;
using dish::composer::WakeStateController;
using dish::models::LinkState;
using dish::util::DisplaySleepInhibitor;

namespace {

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

// Mirrors how AppModel wires the wake subsystem.
struct WakeHarness {
    Observable<int> count{0};
    Observable<int> keepOn{0};
    WakeStateComposer composer{count, keepOn};
    FakeInhibitor fake;
    WakeStateController controller{composer.state(), &fake, QStringLiteral("test reason")};

    WakeHarness() { controller.start(); }
};

} // namespace

TEST_CASE("wake: first stream acquires inhibitor", "[wake]") {
    WakeHarness h;
    h.count.set(1);
    REQUIRE(h.fake.acquires() == 1);
    REQUIRE(h.fake.releases() == 0);
    REQUIRE(h.fake.isHeld());
    REQUIRE(h.fake.lastReason() == "test reason");
}

TEST_CASE("wake: 1 -> 2 slots does not re-acquire", "[wake]") {
    WakeHarness h;
    h.count.set(1);
    h.count.set(2);
    REQUIRE(h.fake.acquires() == 1);
}

TEST_CASE("wake: positive -> 0 releases", "[wake]") {
    WakeHarness h;
    h.count.set(2);
    h.count.set(0);
    REQUIRE(h.fake.acquires() == 1);
    REQUIRE(h.fake.releases() == 1);
    REQUIRE_FALSE(h.fake.isHeld());
}

TEST_CASE("wake: staying at 0 is idempotent", "[wake]") {
    WakeHarness h;
    h.count.set(0); // no change from the initial 0
    REQUIRE(h.fake.acquires() == 0);
    REQUIRE(h.fake.releases() == 0);
}

TEST_CASE("wake: re-acquires after a drop", "[wake]") {
    WakeHarness h;
    h.count.set(1);
    h.count.set(0);
    h.count.set(1);
    REQUIRE(h.fake.acquires() == 2);
    REQUIRE(h.fake.releases() == 1);
    REQUIRE(h.fake.isHeld());
}

TEST_CASE("wake: stop releases the inhibitor (deliberate teardown)", "[wake]") {
    WakeHarness h;
    h.count.set(3);
    h.controller.stop();
    REQUIRE(h.fake.releases() == 1);
    REQUIRE_FALSE(h.fake.isHeld());
}

TEST_CASE("wake: tolerates a null inhibitor", "[wake]") {
    // A headless or stripped-down build can pass nullptr; the controller must
    // still bookkeep without crashing.
    Observable<int> count{0};
    Observable<int> keepOn{0};
    WakeStateComposer composer{count, keepOn};
    WakeStateController controller{composer.state(), nullptr};
    controller.start();
    count.set(1);
    count.set(0);
    controller.stop();
    REQUIRE_FALSE(controller.isInhibiting());
}
