// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pins the pure heartbeat-RTT latency window: the ping-clock arming rule
// (in-flight guard + 5 s loss reclaim), the 64-sample sliding window with its
// validity clamp, the median/2 one-way estimate (android's nearest-rank
// quantile — upper-middle for an even count), and the deterministic "~3.4 ms"
// display formatting. Pure, no Qt, no clock. Replicates the policy dish-android
// hotpath_latency.cpp + LatencyPanel pin for the diagnostics readout (#138).

#include "Util/LatencyWindow.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::formatLatencyMs;
using dish::reducer::kLatencyRttMaxUs;
using dish::reducer::kLatencyWindowCapacity;
using dish::reducer::LatencyWindow;
using dish::reducer::shouldArmPing;

// ── shouldArmPing: in-flight guard + loss reclaim ─────────────────────────────

TEST_CASE("shouldArmPing: arms when no ping is outstanding", "[latency]") {
    REQUIRE(shouldArmPing(0, 1'000'000));
}

TEST_CASE("shouldArmPing: holds the clock while a ping is in flight", "[latency]") {
    // Overwriting would pair the in-flight ping's late ack with the newer stamp
    // and read artificially low.
    REQUIRE_FALSE(shouldArmPing(1'000'000, 1'000'001));
    REQUIRE_FALSE(shouldArmPing(1'000'000, 1'000'000 + kLatencyRttMaxUs - 1));
}

TEST_CASE("shouldArmPing: reclaims a lost ping past the validity window", "[latency]") {
    // Past 5 s the ping is lost, not in flight — the next send re-arms.
    REQUIRE(shouldArmPing(1'000'000, 1'000'000 + kLatencyRttMaxUs));
    REQUIRE(shouldArmPing(1'000'000, 1'000'000 + 2 * kLatencyRttMaxUs));
}

// ── Window: count + median/2 ──────────────────────────────────────────────────

TEST_CASE("window: empty reads zero samples and zero latency", "[latency]") {
    LatencyWindow w;
    REQUIRE(w.count() == 0);
    REQUIRE(w.oneWayP50Ms() == 0.0);
}

TEST_CASE("window: a single RTT sample reads as its half", "[latency]") {
    LatencyWindow w;
    w.push(6.8);
    REQUIRE(w.count() == 1);
    REQUIRE(w.oneWayP50Ms() == 3.4);
}

TEST_CASE("window: odd count takes the middle sample", "[latency]") {
    LatencyWindow w;
    w.push(10.0);
    w.push(2.0);
    w.push(4.0);
    // sorted {2, 4, 10} -> median 4 -> one-way 2.
    REQUIRE(w.count() == 3);
    REQUIRE(w.oneWayP50Ms() == 2.0);
}

TEST_CASE("window: even count takes the upper middle (android nearest-rank)", "[latency]") {
    LatencyWindow w;
    w.push(2.0);
    w.push(4.0);
    // android's q(0.50) indexes round(0.5 * (n - 1)) of the sorted window: for
    // n=2 that is sample 1 (the upper middle), NOT the pair's mean.
    REQUIRE(w.oneWayP50Ms() == 2.0);
    w.push(1.0);
    w.push(3.0);
    // sorted {1, 2, 3, 4}, n=4 -> index 2 -> 3 -> one-way 1.5.
    REQUIRE(w.oneWayP50Ms() == 1.5);
}

TEST_CASE("window: slides at capacity, evicting the oldest sample", "[latency]") {
    LatencyWindow w;
    w.push(1000.0); // will be evicted by the 64 pushes below
    for (int i = 0; i < kLatencyWindowCapacity; ++i) { w.push(10.0); }
    REQUIRE(w.count() == kLatencyWindowCapacity);
    // The 1000 ms outlier slid out, so the median answers "now": all-10 -> 5.
    REQUIRE(w.oneWayP50Ms() == 5.0);
}

TEST_CASE("window: rejects out-of-range samples", "[latency]") {
    LatencyWindow w;
    w.push(-0.1);   // clock retrograde
    w.push(5000.0); // at the loss cap — a reclaimed ping's stale ack
    w.push(6000.0); // beyond it
    REQUIRE(w.count() == 0);
    w.push(4999.9); // just inside stays
    REQUIRE(w.count() == 1);
}

TEST_CASE("window: reset drops every sample", "[latency]") {
    LatencyWindow w;
    w.push(10.0);
    w.push(20.0);
    w.reset();
    REQUIRE(w.count() == 0);
    REQUIRE(w.oneWayP50Ms() == 0.0);
    // Fresh pushes measure the new session only.
    w.push(4.0);
    REQUIRE(w.oneWayP50Ms() == 2.0);
}

// ── formatLatencyMs: deterministic one-decimal display ────────────────────────

TEST_CASE("formatLatencyMs: one decimal, half away from zero", "[latency]") {
    REQUIRE(formatLatencyMs(3.4) == "~3.4 ms");
    REQUIRE(formatLatencyMs(3.44) == "~3.4 ms");
    REQUIRE(formatLatencyMs(12.06) == "~12.1 ms");
    REQUIRE(formatLatencyMs(5.0) == "~5.0 ms");
    REQUIRE(formatLatencyMs(0.24) == "~0.2 ms");
}

TEST_CASE("formatLatencyMs: never negative", "[latency]") {
    REQUIRE(formatLatencyMs(0.0) == "~0.0 ms");
    REQUIRE(formatLatencyMs(-3.0) == "~0.0 ms");
}
