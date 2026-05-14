// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/DisplaySleepInhibitor.h"
#include "Util/ScreenWakeController.h"

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QString>

using dish::models::ConnectionLive;
using dish::util::DisplaySleepInhibitor;
using dish::util::ScreenWakeController;

namespace {

// A fake inhibitor that records the acquire/release lifecycle. The real
// FreedesktopScreenSaverInhibitor requires a running session bus, which is
// usually unavailable in CI containers — a fake keeps the tests self-contained.
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

// ---------------------------------------------------------------------------
// streamingCount — pure derivation
// ---------------------------------------------------------------------------

TEST_CASE("streamingCount: zero when nothing is bound", "[wake]") {
    QHash<QString, QString> bindings;
    QHash<QString, ConnectionLive> states;
    states.insert("c1", ConnectionLive::Connected);
    REQUIRE(ScreenWakeController::streamingCount(bindings, states) == 0);
}

TEST_CASE("streamingCount: ignores bindings to idle / connecting", "[wake]") {
    QHash<QString, QString> bindings{
        {"slot-a", "conn-1"}, {"slot-b", "conn-2"}, {"slot-c", "conn-3"}};
    QHash<QString, ConnectionLive> states{
        {"conn-1", ConnectionLive::Idle},
        {"conn-2", ConnectionLive::Connecting},
        {"conn-3", ConnectionLive::Connected},
    };
    REQUIRE(ScreenWakeController::streamingCount(bindings, states) == 1);
}

TEST_CASE("streamingCount: counts multiple connected slots", "[wake]") {
    QHash<QString, QString> bindings{{"a", "c1"}, {"b", "c2"}, {"c", "c3"}};
    QHash<QString, ConnectionLive> states{
        {"c1", ConnectionLive::Connected},
        {"c2", ConnectionLive::Connected},
        {"c3", ConnectionLive::Idle},
    };
    REQUIRE(ScreenWakeController::streamingCount(bindings, states) == 2);
}

TEST_CASE("streamingCount: unknown connection counts as idle", "[wake]") {
    QHash<QString, QString> bindings{{"a", "missing"}};
    QHash<QString, ConnectionLive> states;
    REQUIRE(ScreenWakeController::streamingCount(bindings, states) == 0);
}

// ---------------------------------------------------------------------------
// update() drives the inhibitor on 0↔positive transitions
// ---------------------------------------------------------------------------

TEST_CASE("update: first stream acquires inhibitor", "[wake]") {
    FakeInhibitor fake;
    ScreenWakeController c(&fake, QStringLiteral("test reason"));
    c.update(1);
    REQUIRE(fake.acquires() == 1);
    REQUIRE(fake.releases() == 0);
    REQUIRE(fake.isHeld());
    REQUIRE(fake.lastReason() == "test reason");
}

TEST_CASE("update: 1 → 2 slots does not re-acquire", "[wake]") {
    FakeInhibitor fake;
    ScreenWakeController c(&fake);
    c.update(1);
    c.update(2);
    REQUIRE(fake.acquires() == 1);
}

TEST_CASE("update: positive → 0 releases", "[wake]") {
    FakeInhibitor fake;
    ScreenWakeController c(&fake);
    c.update(2);
    c.update(0);
    REQUIRE(fake.acquires() == 1);
    REQUIRE(fake.releases() == 1);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("update: staying at 0 is idempotent", "[wake]") {
    FakeInhibitor fake;
    ScreenWakeController c(&fake);
    c.update(0);
    c.update(0);
    REQUIRE(fake.acquires() == 0);
    REQUIRE(fake.releases() == 0);
}

TEST_CASE("update: re-acquires after a drop", "[wake]") {
    FakeInhibitor fake;
    ScreenWakeController c(&fake);
    c.update(1);
    c.update(0);
    c.update(1);
    REQUIRE(fake.acquires() == 2);
    REQUIRE(fake.releases() == 1);
    REQUIRE(fake.isHeld());
}

TEST_CASE("reset: releases and zeros the count", "[wake]") {
    FakeInhibitor fake;
    ScreenWakeController c(&fake);
    c.update(3);
    c.reset();
    REQUIRE(c.streamingSlotCount() == 0);
    REQUIRE(fake.releases() == 1);
    REQUIRE_FALSE(fake.isHeld());
}

TEST_CASE("ScreenWakeController tolerates a null inhibitor", "[wake]") {
    // Defensive: a future build flag or stripped-down packaging may pass
    // nullptr (e.g. headless). The controller must still bookkeep its count
    // without crashing — important because the same instance is then used by
    // every connect/disconnect transition in the AppModel.
    ScreenWakeController c(nullptr);
    c.update(1);
    c.update(0);
    c.reset();
    REQUIRE(c.streamingSlotCount() == 0);
}
