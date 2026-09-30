// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The stick tests' math, driven by synthetic sweeps. The cases mirror
// dish-android's StickHealthTest, so the two clients hold the same numbers.

#include "core/input/StickHealth.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;
using dish::input::circularityError;
using dish::input::kDriftCaptureMs;
using dish::input::kRangeCaptureMs;
using dish::input::kRimBuckets;
using dish::input::kRimBucketsForVerdict;
using dish::input::rimBucketOf;
using dish::input::RimSweep;
using dish::input::StickBench;
using dish::input::stickDrift;
using dish::input::StickEnvelope;
using dish::input::stickEnvelope;
using dish::input::StickSample;
using dish::input::StickTest;
using dish::input::StickTestKind;
using dish::input::StickTestPhase;
using dish::input::StickTestResult;
using dish::input::suggestedDeadzone;
using dish::input::worstReach;

namespace {

constexpr double kDegreesPerHalfTurn = 180.0;
constexpr double kPi = 3.14159265358979323846;

StickSample onCircle(int degrees, float radius) {
    const double rad = static_cast<double>(degrees) * kPi / kDegreesPerHalfTurn;
    return {static_cast<float>(std::cos(rad)) * radius, static_cast<float>(std::sin(rad)) * radius};
}

std::vector<StickSample> circle(float radius) {
    std::vector<StickSample> out;
    for (int degrees = 0; degrees < 360; ++degrees) { out.push_back(onCircle(degrees, radius)); }
    return out;
}

// The right-hand quadrant only reaches 70% of the rail: a worn stick.
std::vector<StickSample> circleWithFlatRight() {
    std::vector<StickSample> out;
    for (int degrees = 0; degrees < 360; ++degrees) {
        const bool rightQuadrant = degrees < 45 || degrees > 315;
        out.push_back(onCircle(degrees, rightQuadrant ? 0.7F : 1.0F));
    }
    return out;
}

void feed(StickTest& test, const std::vector<StickSample>& left,
          const std::vector<StickSample>& right) {
    for (std::size_t i = 0; i < left.size(); ++i) { test.add(left[i], right[i]); }
}

} // namespace

TEST_CASE("the drift of an empty capture is zero", "[diagnostics][stick]") {
    REQUIRE(stickDrift({}) == 0.0F);
}

TEST_CASE("drift averages the resting offset's distance from centre", "[diagnostics][stick]") {
    const std::vector<StickSample> resting(10, StickSample{0.06F, 0.08F});
    REQUIRE(stickDrift(resting) == Approx(0.1F).epsilon(0.0001));
}

TEST_CASE("the suggested dead zone gives headroom over the drift, in whole percent",
          "[diagnostics][stick]") {
    REQUIRE(suggestedDeadzone(0.10F) == Approx(0.15F));
    REQUIRE(suggestedDeadzone(0.071F) == Approx(0.11F));
}

TEST_CASE("the suggested dead zone is floored at sensor noise and capped for a faulty stick",
          "[diagnostics][stick]") {
    REQUIRE(suggestedDeadzone(0.0F) == Approx(0.04F));
    REQUIRE(suggestedDeadzone(0.9F) == Approx(0.30F));
}

TEST_CASE("the envelope tracks each axis' extremes", "[diagnostics][stick]") {
    const StickEnvelope e =
        stickEnvelope({{-0.9F, 0.1F}, {0.95F, 0.0F}, {0.0F, -0.85F}, {0.1F, 0.92F}});
    REQUIRE(e.minX == -0.9F);
    REQUIRE(e.maxX == 0.95F);
    REQUIRE(e.minY == -0.85F);
    REQUIRE(e.maxY == 0.92F);
}

TEST_CASE("a clean full circle has almost no circularity error", "[diagnostics][stick]") {
    const auto error = stickEnvelope(circle(0.98F)).circularityError;
    REQUIRE(error.has_value());
    REQUIRE(*error < 0.01F);
}

TEST_CASE("a flat spot on one side shows as circularity error", "[diagnostics][stick]") {
    const auto error = stickEnvelope(circleWithFlatRight()).circularityError;
    REQUIRE(error.has_value());
    REQUIRE(*error > 0.15F);
}

TEST_CASE("an unfinished sweep gives no circularity verdict", "[diagnostics][stick]") {
    // Only the top-right arc: most buckets are never visited.
    std::vector<StickSample> arc;
    for (int degrees = 0; degrees < 60; ++degrees) { arc.push_back(onCircle(degrees, 1.0F)); }
    REQUIRE_FALSE(stickEnvelope(arc).circularityError.has_value());
}

TEST_CASE("inner travel does not count toward the rim", "[diagnostics][stick]") {
    // A full circle at 30% is the user wiggling the stick, not its gate.
    REQUIRE_FALSE(stickEnvelope(circle(0.3F)).circularityError.has_value());
}

TEST_CASE("a rim that never moved from centre gives no verdict", "[diagnostics][stick]") {
    RimSweep sweep;
    sweep.seen.fill(true);
    REQUIRE_FALSE(circularityError(sweep).has_value());
}

TEST_CASE("a sweep one bucket short of a verdict gives none, and one more gives one",
          "[diagnostics][stick]") {
    RimSweep sweep;
    sweep.reach.fill(1.0F);
    const auto shortOfVerdict = static_cast<std::size_t>(kRimBucketsForVerdict - 1);
    for (std::size_t i = 0; i < shortOfVerdict; ++i) { sweep.seen[i] = true; }
    REQUIRE_FALSE(circularityError(sweep).has_value());
    sweep.seen[shortOfVerdict] = true;
    REQUIRE(circularityError(sweep).has_value());
}

TEST_CASE("the rim buckets run round the circle from straight left", "[diagnostics][stick]") {
    // Straight left sits on the seam where the angle wraps, and must land in
    // the last bucket rather than one past it.
    REQUIRE(rimBucketOf({-1.0F, 0.0F}) == static_cast<std::size_t>(kRimBuckets - 1));
    REQUIRE(rimBucketOf({0.0F, -1.0F}) == static_cast<std::size_t>(kRimBuckets / 4));
    REQUIRE(rimBucketOf({1.0F, 0.0F}) == static_cast<std::size_t>(kRimBuckets / 2));
    REQUIRE(rimBucketOf({0.0F, 1.0F}) == static_cast<std::size_t>(3 * kRimBuckets / 4));
}

TEST_CASE("the worst rail is the one the stick reaches least", "[diagnostics][stick]") {
    const StickEnvelope e{-0.9F, 0.95F, -0.7F, 0.92F, std::nullopt};
    REQUIRE(worstReach(e) == 0.7F);
}

TEST_CASE("a rail never reached in its own direction floors the reach at zero",
          "[diagnostics][stick]") {
    const StickEnvelope e{0.1F, 0.95F, -0.7F, 0.92F, std::nullopt};
    REQUIRE(worstReach(e) == 0.0F);
}

TEST_CASE("a running test counts one above its whole seconds left", "[diagnostics][stick]") {
    const StickTest test(StickTestKind::Drift, 1'000);
    REQUIRE(test.secondsLeftAt(1'500) == 3);
    REQUIRE(test.secondsLeftAt(2'000) == 3);
    REQUIRE(test.secondsLeftAt(3'999) == 1);
}

TEST_CASE("a test finishes when its capture time runs out", "[diagnostics][stick]") {
    const StickTest drift(StickTestKind::Drift, 0);
    REQUIRE_FALSE(drift.finishedAt(kDriftCaptureMs - 1));
    REQUIRE(drift.finishedAt(kDriftCaptureMs));
    REQUIRE(drift.finishedAt(kDriftCaptureMs + 40));

    const StickTest range(StickTestKind::Range, 0);
    REQUIRE_FALSE(range.finishedAt(kRangeCaptureMs - 1));
    REQUIRE(range.finishedAt(kRangeCaptureMs));
}

TEST_CASE("a drift test reports both sticks and a dead zone for the worse one",
          "[diagnostics][stick]") {
    StickTest test(StickTestKind::Drift, 0);
    feed(test, std::vector<StickSample>(20, StickSample{0.06F, 0.08F}),
         std::vector<StickSample>(20, StickSample{0.0F, 0.2F}));
    const StickTestResult r = test.result();
    REQUIRE(r.kind == StickTestKind::Drift);
    REQUIRE(r.driftLeft == Approx(0.1F).epsilon(0.0001));
    REQUIRE(r.driftRight == Approx(0.2F).epsilon(0.0001));
    REQUIRE(r.suggestedDeadzone == Approx(0.30F));
}

TEST_CASE("a range test reports each stick's reach and roundness", "[diagnostics][stick]") {
    StickTest test(StickTestKind::Range, 0);
    feed(test, circle(1.0F), circleWithFlatRight());
    const StickTestResult r = test.result();
    REQUIRE(r.kind == StickTestKind::Range);
    REQUIRE(r.reachLeft == Approx(1.0F).epsilon(0.001));
    // The flat quadrant's edges still reach the rail at 45 degrees, cos 45 out.
    REQUIRE(r.reachRight == Approx(0.7071F).epsilon(0.001));
    REQUIRE(r.circularityLeft.has_value());
    REQUIRE(*r.circularityLeft < 0.01F);
    REQUIRE(r.circularityRight.has_value());
    REQUIRE(*r.circularityRight > 0.15F);
}

TEST_CASE("a finished test counts the polls it captured", "[diagnostics][stick]") {
    StickTest test(StickTestKind::Range, 0);
    feed(test, circle(1.0F), circle(1.0F));
    REQUIRE(test.result().samples == 360);
}

TEST_CASE("the bench is idle until a test starts", "[diagnostics][stick]") {
    const StickBench bench;
    REQUIRE(bench.phase() == StickTestPhase::Idle);
    REQUIRE_FALSE(bench.result().has_value());
}

TEST_CASE("a started test runs, counting down, until its capture time ends",
          "[diagnostics][stick]") {
    StickBench bench;
    bench.start(StickTestKind::Range, 1'000);
    bench.sample({1.0F, 0.0F}, {0.0F, 1.0F});
    bench.advance(2'000);
    REQUIRE(bench.phase() == StickTestPhase::Running);
    REQUIRE(bench.kind() == StickTestKind::Range);
    REQUIRE(bench.secondsLeftAt(2'000) == 8);
    REQUIRE_FALSE(bench.result().has_value());
}

TEST_CASE("the poll that reaches a test's end leaves its result", "[diagnostics][stick]") {
    StickBench bench;
    bench.start(StickTestKind::Drift, 0);
    for (int i = 0; i < 5; ++i) { bench.sample({0.06F, 0.08F}, {0.0F, 0.0F}); }
    bench.advance(kDriftCaptureMs);
    REQUIRE(bench.phase() == StickTestPhase::Done);
    REQUIRE(bench.kind() == StickTestKind::Drift);
    REQUIRE(bench.secondsLeftAt(kDriftCaptureMs) == 0);
    REQUIRE(bench.result().has_value());
    REQUIRE(bench.result()->samples == 5);
    REQUIRE(bench.result()->driftLeft == Approx(0.1F).epsilon(0.0001));
}

TEST_CASE("the bench takes no samples while no test runs", "[diagnostics][stick]") {
    StickBench bench;
    bench.sample({0.5F, 0.5F}, {0.5F, 0.5F});
    bench.start(StickTestKind::Drift, 0);
    bench.advance(kDriftCaptureMs);
    REQUIRE(bench.result().has_value());
    REQUIRE(bench.result()->samples == 0);
}

TEST_CASE("starting a test puts the last result behind the running one", "[diagnostics][stick]") {
    StickBench bench;
    bench.start(StickTestKind::Drift, 0);
    bench.advance(kDriftCaptureMs);
    bench.start(StickTestKind::Range, kDriftCaptureMs);
    REQUIRE(bench.phase() == StickTestPhase::Running);
    REQUIRE(bench.kind() == StickTestKind::Range);
}
