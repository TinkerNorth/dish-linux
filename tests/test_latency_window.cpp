// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/LatencyWindow.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::formatLatencyMs;
using dish::reducer::kLatencyRttMaxUs;
using dish::reducer::kLatencyWindowCapacity;
using dish::reducer::LatencySummary;
using dish::reducer::LatencyWindow;
using dish::reducer::shouldArmPing;

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
    // q(0.50) indexes round(0.5 * (n - 1)) of the sorted window: for n=2 that
    // is sample 1 (the upper middle), NOT the pair's mean.
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
    // The 1000 ms outlier slid out: all-10 -> 5.
    REQUIRE(w.oneWayP50Ms() == 5.0);
}

TEST_CASE("window: rejects out-of-range samples", "[latency]") {
    LatencyWindow w;
    w.push(-0.1);   // clock retrograde
    w.push(5000.0); // at the loss cap — a reclaimed ping's stale ack
    w.push(6000.0);
    REQUIRE(w.count() == 0);
    w.push(4999.9);
    REQUIRE(w.count() == 1);
}

TEST_CASE("window: reset drops every sample", "[latency]") {
    LatencyWindow w;
    w.push(10.0);
    w.push(20.0);
    w.reset();
    REQUIRE(w.count() == 0);
    REQUIRE(w.oneWayP50Ms() == 0.0);
    w.push(4.0);
    REQUIRE(w.oneWayP50Ms() == 2.0);
}

TEST_CASE("window summary: an empty window holds no samples and no round trip", "[latency]") {
    const LatencyWindow w;
    REQUIRE(w.summary() == LatencySummary{});
}

TEST_CASE("window summary: reads the median and the 99th-percentile round trip", "[latency]") {
    LatencyWindow w;
    // Pushed out of order: the quantiles read the sorted window, not arrival.
    for (int i = kLatencyWindowCapacity; i >= 1; --i) { w.push(static_cast<double>(i)); }
    const LatencySummary s = w.summary();
    REQUIRE(s.samples == kLatencyWindowCapacity);
    // Sorted {1..64}: nearest rank round(0.50 * 63) = 32 -> 33, round(0.99 * 63) = 62 -> 63.
    REQUIRE(s.rttP50Ms == 33.0);
    REQUIRE(s.rttP99Ms == 63.0);
}

TEST_CASE("window summary: one sample is both its median and its tail", "[latency]") {
    LatencyWindow w;
    w.push(6.8);
    const LatencySummary s = w.summary();
    REQUIRE(s.samples == 1);
    REQUIRE(s.rttP50Ms == 6.8);
    REQUIRE(s.rttP99Ms == 6.8);
}

TEST_CASE("window summary: the one-way figure is half the median round trip", "[latency]") {
    LatencySummary s;
    s.samples = 3;
    s.rttP50Ms = 6.8;
    s.rttP99Ms = 9.0;
    REQUIRE(s.oneWayMs() == 3.4);
}

TEST_CASE("window summary: the window's one-way figure is its summary's", "[latency]") {
    LatencyWindow w;
    w.push(10.0);
    w.push(2.0);
    w.push(4.0);
    REQUIRE(w.oneWayP50Ms() == w.summary().oneWayMs());
}

TEST_CASE("formatLatencyMs: one decimal, half away from zero", "[latency]") {
    REQUIRE(formatLatencyMs(3.4) == "~3.4 ms");
    REQUIRE(formatLatencyMs(3.44) == "~3.4 ms");
    REQUIRE(formatLatencyMs(12.06) == "~12.1 ms");
    REQUIRE(formatLatencyMs(5.0) == "~5.0 ms");
}

// "~0.0 ms" would claim a latency a network link cannot have.
TEST_CASE("formatLatencyMs: sub-millisecond reads as a bound, not a zero", "[latency]") {
    REQUIRE(formatLatencyMs(0.24) == "<1 ms");
    REQUIRE(formatLatencyMs(0.94) == "<1 ms");
    // The boundary rounds UP into the figure: 0.95 -> 1.0.
    REQUIRE(formatLatencyMs(0.95) == "~1.0 ms");
    REQUIRE(formatLatencyMs(1.0) == "~1.0 ms");
}

TEST_CASE("formatLatencyMs: never negative", "[latency]") {
    REQUIRE(formatLatencyMs(0.0) == "<1 ms");
    REQUIRE(formatLatencyMs(-3.0) == "<1 ms");
}
