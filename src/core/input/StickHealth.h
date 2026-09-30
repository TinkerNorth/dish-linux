// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The input inspector's two stick tests, as pure math over captured samples:
// the drift test averages how far a stick rests from centre when nobody is
// touching it, and the range test measures how far it reaches and how round its
// gate is. dish-android's StickHealth.kt holds the same numbers, so the two
// clients read one stick the same way.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace dish::input {

// One stick reading, each axis a fraction of full travel (-1..1).
struct StickSample {
    float x = 0.0F;
    float y = 0.0F;
};

inline float stickMagnitude(const StickSample& s) { return std::hypot(s.x, s.y); }

// The mean resting offset: the stick was supposed to be untouched.
inline float stickDrift(const std::vector<StickSample>& samples) {
    if (samples.empty()) { return 0.0F; }
    float total = 0.0F;
    for (const auto& s : samples) { total += stickMagnitude(s); }
    return total / static_cast<float>(samples.size());
}

// A dead zone with headroom over the measured drift, floored at sensor noise and
// capped where a stick is faulty rather than drifting, in whole percent.
inline constexpr float kDeadzoneHeadroom = 1.5F;
inline constexpr float kDeadzoneFloor = 0.04F;
inline constexpr float kDeadzoneCeiling = 0.30F;
inline constexpr float kPercent = 100.0F;

inline float suggestedDeadzone(float drift) {
    const float withHeadroom = drift * kDeadzoneHeadroom;
    const float bounded = std::clamp(withHeadroom, kDeadzoneFloor, kDeadzoneCeiling);
    return std::round(bounded * kPercent) / kPercent;
}

// Angle buckets for the roundness sweep: coarse enough that an ordinary hand
// sweep fills every one, fine enough to catch a flat spot on one side.
inline constexpr int kRimBuckets = 16;
// Only a sample this far out describes the gate; anything closer in is where the
// stick happened to be, not how far it can go.
inline constexpr float kRimThreshold = 0.5F;
// Fewer buckets visited than this and the user never finished the circle, so a
// roundness figure would be noise.
inline constexpr int kRimBucketsForVerdict = 12;

// The furthest the stick reached in each direction around the circle.
struct RimSweep {
    std::array<float, kRimBuckets> reach{};
    std::array<bool, kRimBuckets> seen{};
};

inline std::size_t rimBucketOf(const StickSample& s) {
    const double angle = std::atan2(static_cast<double>(s.y), static_cast<double>(s.x));
    const double turn = (angle + std::numbers::pi) / (2.0 * std::numbers::pi);
    const auto bucket = static_cast<int>(turn * kRimBuckets);
    return static_cast<std::size_t>(std::clamp(bucket, 0, kRimBuckets - 1));
}

inline void addToRim(RimSweep& sweep, const StickSample& s) {
    const float reach = stickMagnitude(s);
    const bool atTheRim = reach >= kRimThreshold;
    if (!atTheRim) { return; }
    const std::size_t bucket = rimBucketOf(s);
    sweep.seen[bucket] = true;
    sweep.reach[bucket] = std::max(sweep.reach[bucket], reach);
}

struct RimCoverage {
    int buckets = 0;
    float totalReach = 0.0F;
};

inline RimCoverage rimCoverage(const RimSweep& sweep) {
    RimCoverage coverage;
    for (std::size_t i = 0; i < sweep.seen.size(); ++i) {
        if (!sweep.seen[i]) { continue; }
        ++coverage.buckets;
        coverage.totalReach += sweep.reach[i];
    }
    return coverage;
}

inline float worstRimDeviation(const RimSweep& sweep, float meanReach) {
    float worst = 0.0F;
    for (std::size_t i = 0; i < sweep.seen.size(); ++i) {
        if (!sweep.seen[i]) { continue; }
        worst = std::max(worst, std::abs(sweep.reach[i] - meanReach));
    }
    return worst;
}

// The largest deviation from the mean rim reach, as a fraction of it. Empty
// until the sweep covered enough of the circle, or when the rim never moved.
inline std::optional<float> circularityError(const RimSweep& sweep) {
    const RimCoverage coverage = rimCoverage(sweep);
    const bool sweptEnough = coverage.buckets >= kRimBucketsForVerdict;
    if (!sweptEnough) { return std::nullopt; }
    const float meanReach = coverage.totalReach / static_cast<float>(coverage.buckets);
    const bool rimMoved = meanReach > 0.0F;
    if (!rimMoved) { return std::nullopt; }
    return worstRimDeviation(sweep, meanReach) / meanReach;
}

struct StickEnvelope {
    float minX = 0.0F;
    float maxX = 0.0F;
    float minY = 0.0F;
    float maxY = 0.0F;
    std::optional<float> circularityError;
};

inline StickEnvelope stickEnvelope(const std::vector<StickSample>& samples) {
    StickEnvelope envelope;
    RimSweep rim;
    for (const auto& s : samples) {
        envelope.minX = std::min(envelope.minX, s.x);
        envelope.maxX = std::max(envelope.maxX, s.x);
        envelope.minY = std::min(envelope.minY, s.y);
        envelope.maxY = std::max(envelope.maxY, s.y);
        addToRim(rim, s);
    }
    envelope.circularityError = circularityError(rim);
    return envelope;
}

// The rail the stick struggles to reach is the one that matters in a game.
inline float worstReach(const StickEnvelope& e) {
    const float reach = std::min({-e.minX, e.maxX, -e.minY, e.maxY});
    return std::max(reach, 0.0F);
}

enum class StickTestKind : std::uint8_t { Drift, Range };

// How long each test captures for: drift needs the stick at rest for a moment,
// range needs time to sweep both sticks round.
inline constexpr std::int64_t kDriftCaptureMs = 3000;
inline constexpr std::int64_t kRangeCaptureMs = 8000;
inline constexpr std::int64_t kMsPerSecond = 1000;

inline std::int64_t captureMsFor(StickTestKind kind) {
    switch (kind) {
    case StickTestKind::Drift:
        return kDriftCaptureMs;
    case StickTestKind::Range:
        return kRangeCaptureMs;
    }
    return kDriftCaptureMs;
}

struct StickTestResult {
    StickTestKind kind = StickTestKind::Drift;
    // How many polls the capture took; none means the pad sent nothing and the
    // figures below measure nothing.
    int samples = 0;
    float driftLeft = 0.0F;
    float driftRight = 0.0F;
    float suggestedDeadzone = 0.0F;
    float reachLeft = 0.0F;
    float reachRight = 0.0F;
    std::optional<float> circularityLeft;
    std::optional<float> circularityRight;
};

// One running test: both sticks' samples, and when the capture ends. The clock
// is passed in, so the whole test runs on virtual time.
class StickTest {
  public:
    StickTest(StickTestKind kind, std::int64_t startMs)
        : kind_(kind), endsAtMs_(startMs + captureMsFor(kind)) {}

    StickTestKind kind() const { return kind_; }

    void add(const StickSample& left, const StickSample& right) {
        left_.push_back(left);
        right_.push_back(right);
    }

    bool finishedAt(std::int64_t nowMs) const { return nowMs >= endsAtMs_; }

    // Whole seconds left plus one, so a running capture never reads zero while
    // it is still taking samples.
    int secondsLeftAt(std::int64_t nowMs) const {
        const std::int64_t leftMs = endsAtMs_ - nowMs;
        return static_cast<int>(leftMs / kMsPerSecond + 1);
    }

    StickTestResult result() const {
        switch (kind_) {
        case StickTestKind::Drift:
            return driftResult();
        case StickTestKind::Range:
            return rangeResult();
        }
        return driftResult();
    }

  private:
    // The dead zone is sized for the worse stick: one setting covers both.
    StickTestResult driftResult() const {
        StickTestResult r;
        r.kind = StickTestKind::Drift;
        r.samples = static_cast<int>(left_.size());
        r.driftLeft = stickDrift(left_);
        r.driftRight = stickDrift(right_);
        r.suggestedDeadzone = suggestedDeadzone(std::max(r.driftLeft, r.driftRight));
        return r;
    }

    StickTestResult rangeResult() const {
        const StickEnvelope left = stickEnvelope(left_);
        const StickEnvelope right = stickEnvelope(right_);
        StickTestResult r;
        r.kind = StickTestKind::Range;
        r.samples = static_cast<int>(left_.size());
        r.reachLeft = worstReach(left);
        r.reachRight = worstReach(right);
        r.circularityLeft = left.circularityError;
        r.circularityRight = right.circularityError;
        return r;
    }

    StickTestKind kind_;
    std::int64_t endsAtMs_;
    std::vector<StickSample> left_;
    std::vector<StickSample> right_;
};

enum class StickTestPhase : std::uint8_t { Idle, Running, Done };

// The inspector's one stick-test slot: nothing yet, a test running, or the last
// test's result. Starting a test replaces whatever was there.
class StickBench {
  public:
    void start(StickTestKind kind, std::int64_t nowMs) { running_.emplace(kind, nowMs); }

    // One poll's sticks, taken only while a test runs.
    void sample(const StickSample& left, const StickSample& right) {
        if (running_.has_value()) { running_->add(left, right); }
    }

    // The poll that reaches a running test's end turns it into the result.
    void advance(std::int64_t nowMs) {
        const bool due = running_.has_value() && running_->finishedAt(nowMs);
        if (!due) { return; }
        result_ = running_->result();
        running_.reset();
    }

    StickTestPhase phase() const {
        if (running_.has_value()) { return StickTestPhase::Running; }
        return result_.has_value() ? StickTestPhase::Done : StickTestPhase::Idle;
    }

    // The running test's kind, else the last result's.
    StickTestKind kind() const {
        if (running_.has_value()) { return running_->kind(); }
        return result_.has_value() ? result_->kind : StickTestKind::Drift;
    }

    int secondsLeftAt(std::int64_t nowMs) const {
        return running_.has_value() ? running_->secondsLeftAt(nowMs) : 0;
    }

    const std::optional<StickTestResult>& result() const { return result_; }

  private:
    std::optional<StickTest> running_;
    std::optional<StickTestResult> result_;
};

} // namespace dish::input
