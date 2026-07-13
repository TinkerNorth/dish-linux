// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pure reconcile decision logic (contract §Enriched heartbeat ack): the
// drift trigger ((epoch,bitmap) vs applied), the GET converge decision, and the
// late-slot converge diff. Ports the dish-windows test_session_reconcile cases.

#include "Network/Reconcile.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace reducer = dish::reducer;
using reducer::AppliedSlot;
using reducer::DesiredSlot;

// ── expectedBitmap ──────────────────────────────────────────────────────────

TEST_CASE("expectedBitmap sets one bit per registered controller index", "[reconcile]") {
    REQUIRE(reducer::expectedBitmap({}) == 0);
    REQUIRE(reducer::expectedBitmap({{0, 0}}) == 0x0001);
    REQUIRE(reducer::expectedBitmap({{0, 0}, {2, 0}}) == 0x0005);
    REQUIRE(reducer::expectedBitmap({{15, 0}}) == 0x8000);
    // Out-of-range indices (>15) don't set a bit.
    REQUIRE(reducer::expectedBitmap({{16, 0}}) == 0x0000);
}

// ── reconcileNeeded (the heartbeat-ack drift trigger) ───────────────────────

TEST_CASE("reconcileNeeded: no enriched ack yet -> never", "[reconcile]") {
    // serverEpoch < 0 means no enriched ack has been seen.
    REQUIRE_FALSE(reducer::reconcileNeeded(-1, -1, 3, 0x0001));
}

TEST_CASE("reconcileNeeded: epoch drift triggers", "[reconcile]") {
    REQUIRE(reducer::reconcileNeeded(/*srvEpoch=*/4, /*srvBitmap=*/0x0001,
                                     /*lastEpoch=*/3, /*expected=*/0x0001));
}

TEST_CASE("reconcileNeeded: bitmap drift at matching epoch triggers", "[reconcile]") {
    // The server lost controller 1 (bitmap 0x0001 vs our expected 0x0003).
    REQUIRE(reducer::reconcileNeeded(3, 0x0001, 3, 0x0003));
}

TEST_CASE("reconcileNeeded: epoch+bitmap both match -> no reconcile", "[reconcile]") {
    REQUIRE_FALSE(reducer::reconcileNeeded(3, 0x0003, 3, 0x0003));
}

TEST_CASE("reconcileNeeded: unknown bitmap (<0) skips the bitmap arm", "[reconcile]") {
    // serverBitmap < 0 means unknown; only the epoch arm decides.
    REQUIRE_FALSE(reducer::reconcileNeeded(3, -1, 3, 0x0003));
    REQUIRE(reducer::reconcileNeeded(4, -1, 3, 0x0003));
}

// ── appliedMatchesDesired (the GET converge decision) ───────────────────────

TEST_CASE("appliedMatchesDesired: identical sets match", "[reconcile]") {
    std::vector<DesiredSlot> desired = {{0, 0}, {1, 1}};
    std::vector<AppliedSlot> applied = {{0, 0, true}, {1, 1, true}};
    REQUIRE(reducer::appliedMatchesDesired(desired, applied));
}

TEST_CASE("appliedMatchesDesired: a type mismatch forces re-PUT", "[reconcile]") {
    std::vector<DesiredSlot> desired = {{0, 1}};       // want DS4
    std::vector<AppliedSlot> applied = {{0, 0, true}}; // got Xbox
    REQUIRE_FALSE(reducer::appliedMatchesDesired(desired, applied));
}

TEST_CASE("appliedMatchesDesired: an inactive applied slot is unplugged", "[reconcile]") {
    std::vector<DesiredSlot> desired = {{0, 0}};
    std::vector<AppliedSlot> applied = {{0, 0, false}}; // server says inactive
    REQUIRE_FALSE(reducer::appliedMatchesDesired(desired, applied));
}

TEST_CASE("appliedMatchesDesired: a missing desired slot forces re-PUT", "[reconcile]") {
    std::vector<DesiredSlot> desired = {{0, 0}, {1, 0}};
    std::vector<AppliedSlot> applied = {{0, 0, true}}; // server missing slot 1
    REQUIRE_FALSE(reducer::appliedMatchesDesired(desired, applied));
}

TEST_CASE("appliedMatchesDesired: a mouse-grant mismatch forces re-PUT", "[reconcile]") {
    // Even when slots line up, wants≠granted (the grant is only computed at
    // session PUT) forces the converge.
    std::vector<DesiredSlot> desired = {{0, 0}};
    std::vector<AppliedSlot> applied = {{0, 0, true}};
    REQUIRE(reducer::appliedMatchesDesired(desired, applied, /*mouseMatch=*/true));
    REQUIRE_FALSE(reducer::appliedMatchesDesired(desired, applied, /*mouseMatch=*/false));
}

// ── lateSlotConverge (slots that change during the PUT round-trip) ──────────

TEST_CASE("lateSlotConverge: nothing changed -> no follow-ups", "[reconcile]") {
    std::vector<DesiredSlot> sent = {{0, 0}};
    const auto c = reducer::lateSlotConverge(sent, sent);
    REQUIRE(c.resyncs.empty());
    REQUIRE(c.removes.empty());
}

TEST_CASE("lateSlotConverge: a newly-added slot resyncs", "[reconcile]") {
    std::vector<DesiredSlot> sent = {{0, 0}};
    std::vector<DesiredSlot> desired = {{0, 0}, {1, 1}};
    const auto c = reducer::lateSlotConverge(sent, desired);
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{1});
    REQUIRE(c.removes.empty());
}

TEST_CASE("lateSlotConverge: a changed type resyncs", "[reconcile]") {
    std::vector<DesiredSlot> sent = {{0, 0}};
    std::vector<DesiredSlot> desired = {{0, 1}}; // type changed
    const auto c = reducer::lateSlotConverge(sent, desired);
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{0});
    REQUIRE(c.removes.empty());
}

TEST_CASE("lateSlotConverge: a removed slot deletes", "[reconcile]") {
    std::vector<DesiredSlot> sent = {{0, 0}, {1, 0}};
    std::vector<DesiredSlot> desired = {{0, 0}};
    const auto c = reducer::lateSlotConverge(sent, desired);
    REQUIRE(c.resyncs.empty());
    REQUIRE(c.removes == std::vector<std::uint8_t>{1});
}
