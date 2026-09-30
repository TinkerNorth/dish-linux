// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The sliding-window RTT estimator behind the one-way latency readout. Heartbeat
// RTT samples slide through a fixed window and the displayed figure is the median
// halved (a symmetric-path estimate), so it answers "now" rather than "since the
// session opened". Allocation-free: a fixed ring, and the median sorts a stack
// copy. SatelliteClient supplies the timing; there is no clock inside.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace dish::reducer {

// At the 2s heartbeat cadence this spans about 2 minutes, so the median tracks
// current network conditions.
inline constexpr int kLatencyWindowCapacity = 64;

// A ping unanswered past this is lost, not in flight: the ping clock is reclaimed
// and an RTT at or over it is dropped rather than skewing the window with a
// retransmit-scale outlier.
inline constexpr std::int64_t kLatencyRttMaxUs = 5'000'000;
inline constexpr double kLatencyRttMaxMs = static_cast<double>(kLatencyRttMaxUs) / 1000.0;

// An in-flight ping's clock is kept: overwriting it would pair its late ack with
// a newer ping's stamp and read artificially low. `outstandingUs` is the previous
// stamp, 0 for none in flight.
inline bool shouldArmPing(std::int64_t outstandingUs, std::int64_t nowUs) {
    return outstandingUs == 0 || nowUs - outstandingUs >= kLatencyRttMaxUs;
}

// The one formatting site: Home, Connections and Configure all render from it.
// Built digit-wise so the decimal separator stays '.' whatever the process
// locale; the UI appends its own localized sample-count suffix.
//
// Sub-millisecond reads "<1 ms" rather than "~0.4 ms". A one-way estimate that
// small off a 2s heartbeat is below the measurement's own resolution, and the
// "~0.0 ms" a wired loopback rounds to would claim a latency of zero, which no
// network link can have.
inline std::string formatLatencyMs(double oneWayMs) {
    const long tenths = oneWayMs <= 0.0 ? 0 : std::lround(oneWayMs * 10.0);
    if (tenths < 10) { return "<1 ms"; }
    std::string out = "~";
    out += std::to_string(tenths / 10);
    out += '.';
    out += std::to_string(tenths % 10);
    out += " ms";
    return out;
}

inline constexpr double kLatencyMedianQuantile = 0.50;
inline constexpr double kLatencyTailQuantile = 0.99;

// What the diagnostics page reads off the window: how many round trips it holds,
// and their median and 99th percentile. All zero while it is empty.
struct LatencySummary {
    int samples = 0;
    double rttP50Ms = 0.0;
    double rttP99Ms = 0.0;

    // Half the median round trip: a symmetric-path estimate is all a
    // single-ended measurement can say about one direction.
    double oneWayMs() const { return rttP50Ms / 2.0; }

    bool operator==(const LatencySummary&) const = default;
};

// One instance per session, guarded by the owner's lock; the class itself is
// single-threaded.
class LatencyWindow {
  public:
    // Samples outside [0, kLatencyRttMaxMs) are dropped, so a retrograde clock or
    // a lost-then-answered ping cannot skew the median. NaN fails the >= 0 test
    // and is dropped by the same branch.
    void push(double rttMs) {
        if (!(rttMs >= 0.0) || rttMs >= kLatencyRttMaxMs) { return; }
        samples_[static_cast<std::size_t>(head_)] = rttMs;
        head_ = (head_ + 1) % kLatencyWindowCapacity;
        if (count_ < kLatencyWindowCapacity) { ++count_; }
    }

    // The UI shows this beside the figure so a barely-seeded window reads as
    // tentative.
    int count() const { return count_; }

    // 0 while the window is empty; callers gate the readout on count() > 0.
    double oneWayP50Ms() const { return summary().oneWayMs(); }

    // The nearest-rank quantile (upper-middle for even n) is shared with the
    // other clients, so identical samples render identical figures.
    LatencySummary summary() const {
        LatencySummary out;
        out.samples = count_;
        if (count_ == 0) { return out; }
        std::array<double, kLatencyWindowCapacity> sorted{};
        std::copy(samples_.begin(), samples_.begin() + count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + count_);
        out.rttP50Ms = sorted[nearestRank(kLatencyMedianQuantile)];
        out.rttP99Ms = sorted[nearestRank(kLatencyTailQuantile)];
        return out;
    }

    void reset() {
        head_ = 0;
        count_ = 0;
    }

  private:
    std::size_t nearestRank(double quantile) const {
        return static_cast<std::size_t>(std::lround(quantile * (count_ - 1)));
    }

    std::array<double, kLatencyWindowCapacity> samples_{};
    int head_ = 0;
    int count_ = 0;
};

} // namespace dish::reducer
