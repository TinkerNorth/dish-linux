// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// LatencyWindow — the pure sliding-window RTT estimator behind the per-connection
// one-way latency readout. Heartbeat RTT samples (ping sent → ack received, ms)
// slide through a fixed 64-sample window; the displayed one-way latency is the
// window median halved (symmetric-path estimate), so the figure answers "now",
// not "since the session opened". Mirrors dish-android hotpath_latency.cpp
// (kRttWindow=64, shouldArmPing's in-flight guard + loss reclaim, addRttSample's
// validity clamp) + LatencyPanel (networkOneWayP50Ms = p50/2, sample count shown);
// ported from dish-windows core/reducer/LatencyWindow.h.
//
// Pure C++17, Qt-free, allocation-free: the window is a fixed std::array ring
// and the median sorts a stack copy. There is no clock inside — SatelliteClient
// stamps ping instants from its own monotonic clock and passes the deltas in.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace dish::reducer {

// Sliding-window capacity. At the 2 s heartbeat cadence 64 samples span ~2 min,
// so the median tracks current network conditions (android kRttWindow).
inline constexpr int kLatencyWindowCapacity = 64;

// A ping unanswered past this is lost, not in flight (android kRttMaxNs = 5 s):
// past it the ping clock is reclaimed, and an RTT at/over it is dropped rather
// than skewing the window with a retransmit-scale outlier.
inline constexpr std::int64_t kLatencyRttMaxUs = 5'000'000;
inline constexpr double kLatencyRttMaxMs = static_cast<double>(kLatencyRttMaxUs) / 1000.0;

// Whether the heartbeat sender may stamp a fresh ping clock. Keep an in-flight
// ping's clock: overwriting would pair its late ack with a newer ping's stamp
// and read artificially low. Past the validity window the ping is lost; reclaim.
// `outstandingUs` is the previous stamp (0 = none in flight); both are the same
// monotonic microsecond basis. Mirrors android hotpath::shouldArmPing.
inline bool shouldArmPing(std::int64_t outstandingUs, std::int64_t nowUs) {
    return outstandingUs == 0 || nowUs - outstandingUs >= kLatencyRttMaxUs;
}

// "~3.4 ms" — the displayed one-way figure, rounded to one decimal (half away
// from zero via std::lround, never banker's) and clamped at 0. Built digit-wise
// so the decimal separator is '.' regardless of the process locale; the UI
// layer appends its own localized sample-count suffix.
inline std::string formatLatencyMs(double oneWayMs) {
    const long tenths = oneWayMs <= 0.0 ? 0 : std::lround(oneWayMs * 10.0);
    std::string out = "~";
    out += std::to_string(tenths / 10);
    out += '.';
    out += std::to_string(tenths % 10);
    out += " ms";
    return out;
}

// The RTT window. push() slides full-round-trip samples in (oldest evicted at
// capacity); oneWayP50Ms() reports median/2. One instance per session, guarded
// by the owner's lock — the class itself is single-threaded.
class LatencyWindow {
  public:
    // Record one RTT sample (milliseconds). Samples outside [0, kLatencyRttMaxMs)
    // are dropped — a negative delta (clock retrograde) or a lost-then-answered
    // ping must not skew the median (android addRttSample's validity clamp).
    // NaN fails the >= 0 comparison and is dropped by the same branch.
    void push(double rttMs) {
        if (!(rttMs >= 0.0) || rttMs >= kLatencyRttMaxMs) { return; }
        samples_[static_cast<std::size_t>(head_)] = rttMs;
        head_ = (head_ + 1) % kLatencyWindowCapacity;
        if (count_ < kLatencyWindowCapacity) { ++count_; }
    }

    // RTT samples currently in the window (0..kLatencyWindowCapacity). The UI
    // shows the figure only when this is > 0, so a fresh session reads blank
    // rather than "~0.0 ms".
    int count() const { return count_; }

    // Median RTT over the window, halved (the one-way symmetric-path estimate).
    // 0 while the window is empty — callers gate the readout on count() > 0.
    // Median = the same nearest-rank quantile android's statsJson uses
    // (index round(0.5 * (n - 1)) of the sorted window; upper-middle for even n),
    // so the clients render identical figures from identical samples.
    double oneWayP50Ms() const {
        if (count_ == 0) { return 0.0; }
        std::array<double, kLatencyWindowCapacity> sorted{};
        std::copy(samples_.begin(), samples_.begin() + count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + count_);
        const auto i = static_cast<std::size_t>(std::lround(0.50 * (count_ - 1)));
        return sorted[i] / 2.0;
    }

    // Drop every sample (a new session's window starts fresh).
    void reset() {
        head_ = 0;
        count_ = 0;
    }

  private:
    std::array<double, kLatencyWindowCapacity> samples_{};
    int head_ = 0;
    int count_ = 0;
};

} // namespace dish::reducer
