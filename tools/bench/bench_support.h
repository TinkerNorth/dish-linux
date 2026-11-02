// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

namespace dish::bench {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

inline constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
inline constexpr std::int64_t kNanosPerMicro = 1'000;
inline constexpr std::int64_t kNanosPerMilli = 1'000'000;
inline constexpr std::uint64_t kBasisPoints = 10'000;
inline constexpr std::uint64_t kP50 = 5'000;
inline constexpr std::uint64_t kP99 = 9'900;
inline constexpr std::uint64_t kP999 = 9'990;

// The sequence rides the right stick's X axis above the bridge's stick dead zone, which zeroes
// |v| <= 3277: the values 4096..32767 carry it, so a frame's sequence is its index mod 28672.
inline constexpr std::int32_t kSequenceFloor = 4096;
inline constexpr std::uint32_t kSequencePeriod = 32768U - kSequenceFloor;

constexpr std::uint32_t sequenceOf(std::uint64_t frame) {
    return static_cast<std::uint32_t>(frame % kSequencePeriod);
}

constexpr std::int16_t encodeSequence(std::uint32_t sequence) {
    return static_cast<std::int16_t>(kSequenceFloor +
                                     static_cast<std::int32_t>(sequence % kSequencePeriod));
}

constexpr std::optional<std::uint32_t> decodeSequence(std::int16_t axis) {
    if (axis < kSequenceFloor) { return std::nullopt; }
    return static_cast<std::uint32_t>(axis - kSequenceFloor);
}

// A stamp per sequence, written by one thread and read by another. Slots repeat every Slots
// sequences, which only aliases if that many frames are in flight at once.
template <std::size_t Slots> class SequenceStamps {
  public:
    static_assert(kSequencePeriod % Slots == 0, "a slot must map to one sequence per period");

    void stamp(std::uint32_t sequence, std::int64_t ns) {
        Slot& slot = slots_[sequence % Slots];
        slot.ns.store(ns, std::memory_order_relaxed);
        slot.tag.store(sequence + 1, std::memory_order_release);
    }

    std::optional<std::int64_t> peek(std::uint32_t sequence) const {
        const Slot& slot = slots_[sequence % Slots];
        if (slot.tag.load(std::memory_order_acquire) != sequence + 1) { return std::nullopt; }
        return slot.ns.load(std::memory_order_relaxed);
    }

    // The stamp, once: a second take of the same sequence answers nothing.
    std::optional<std::int64_t> take(std::uint32_t sequence) {
        Slot& slot = slots_[sequence % Slots];
        std::uint32_t tag = sequence + 1;
        if (!slot.tag.compare_exchange_strong(tag, 0, std::memory_order_acq_rel)) {
            return std::nullopt;
        }
        return slot.ns.load(std::memory_order_relaxed);
    }

    void clear() {
        for (Slot& slot : slots_) { slot.tag.store(0, std::memory_order_relaxed); }
    }

  private:
    struct Slot {
        std::atomic<std::uint32_t> tag{0};
        std::atomic<std::int64_t> ns{0};
    };
    std::array<Slot, Slots> slots_{};
};

// The value at a percentile of an ascending list: the smallest value with at least that share of
// the list at or below it.
inline std::int64_t percentileNs(const std::vector<std::int64_t>& sorted,
                                 std::uint64_t basisPoints) {
    if (sorted.empty()) { return 0; }
    const std::uint64_t rank = (sorted.size() * basisPoints + kBasisPoints - 1) / kBasisPoints;
    const std::uint64_t index = rank == 0 ? 0 : rank - 1;
    return sorted[std::min<std::size_t>(static_cast<std::size_t>(index), sorted.size() - 1)];
}

struct LatencySummary {
    std::size_t frames = 0;
    std::int64_t p50Ns = 0;
    std::int64_t p99Ns = 0;
    std::int64_t p999Ns = 0;
    std::int64_t maxNs = 0;
    std::size_t overOneMs = 0;
};

inline LatencySummary summarize(std::vector<std::int64_t> latenciesNs) {
    std::sort(latenciesNs.begin(), latenciesNs.end());
    LatencySummary s;
    s.frames = latenciesNs.size();
    s.p50Ns = percentileNs(latenciesNs, kP50);
    s.p99Ns = percentileNs(latenciesNs, kP99);
    s.p999Ns = percentileNs(latenciesNs, kP999);
    s.maxNs = latenciesNs.empty() ? 0 : latenciesNs.back();
    s.overOneMs = static_cast<std::size_t>(
        std::count_if(latenciesNs.begin(), latenciesNs.end(),
                      [](std::int64_t ns) { return ns > kNanosPerMilli; }));
    return s;
}

// The longest wait between two consecutive arrivals, whatever order they were recorded in.
inline std::int64_t longestGapNs(std::vector<std::int64_t> arrivalsNs) {
    std::sort(arrivalsNs.begin(), arrivalsNs.end());
    std::int64_t longest = 0;
    for (std::size_t i = 1; i < arrivalsNs.size(); ++i) {
        longest = std::max(longest, arrivalsNs[i] - arrivalsNs[i - 1]);
    }
    return longest;
}

// The reported rep: the one whose p99.9 is the median of the reps' (the lower middle for an even
// count, as the satellite's bench picks).
inline std::size_t medianRep(const std::vector<std::int64_t>& p999s) {
    if (p999s.empty()) { return 0; }
    std::vector<std::size_t> order(p999s.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&p999s](std::size_t a, std::size_t b) { return p999s[a] < p999s[b]; });
    return order[(order.size() - 1) / 2];
}

constexpr std::int64_t deadlineOffsetNs(std::uint64_t tick, double rateHz) {
    return static_cast<std::int64_t>(static_cast<double>(tick) *
                                     static_cast<double>(kNanosPerSecond) / rateHz);
}

// One stream's absolute deadlines: frame k is due k periods after the common start, whatever
// happened to frame k - 1.
class Cadence {
  public:
    explicit Cadence(double rateHz) : rateHz_(rateHz) {}

    double rateHz() const { return rateHz_; }
    std::uint64_t tick() const { return tick_; }
    std::int64_t dueNs() const { return deadlineOffsetNs(tick_, rateHz_); }
    void advance() { ++tick_; }

  private:
    double rateHz_;
    std::uint64_t tick_ = 0;
};

// The stream whose next frame is due first.
inline std::size_t earliest(const std::vector<Cadence>& streams) {
    std::size_t first = 0;
    for (std::size_t i = 1; i < streams.size(); ++i) {
        if (streams[i].dueNs() < streams[first].dueNs()) { first = i; }
    }
    return first;
}

// An absolute clock_nanosleep wakes within the 50 us timer slack plus a loaded runner's wake-up
// delay, so a producer asks for 300 us before its deadline and spins the rest.
inline constexpr Nanos kTimerLead = std::chrono::microseconds(300);
inline constexpr std::size_t kMinFramesToJudge = 8;
inline constexpr std::int64_t kMaxMedianLatenessNs = kNanosPerMilli;

struct PacingVerdict {
    std::size_t early = 0;
    std::int64_t medianLatenessNs = 0;
    bool judged = false;
    bool ok = true;
};

// Every frame at or after its deadline, and the median within 1 ms of it. A frame before its
// deadline means the pacer was skipped; a late median means it cannot keep up.
inline PacingVerdict judgePacing(std::vector<std::int64_t> latenessNs) {
    PacingVerdict v;
    v.early = static_cast<std::size_t>(std::count_if(latenessNs.begin(), latenessNs.end(),
                                                     [](std::int64_t ns) { return ns < 0; }));
    std::sort(latenessNs.begin(), latenessNs.end());
    v.medianLatenessNs = percentileNs(latenessNs, kP50);
    v.judged = latenessNs.size() >= kMinFramesToJudge;
    v.ok = !v.judged || (v.early == 0 && v.medianLatenessNs <= kMaxMedianLatenessNs);
    return v;
}

// Lets a receiving thread record only while a window is open, and lets the owner wait out a
// record in progress before it reads what was recorded.
class RecordingGate {
  public:
    bool enter() {
        inFlight_.fetch_add(1);
        if (open_.load()) { return true; }
        inFlight_.fetch_sub(1);
        return false;
    }

    void leave() { inFlight_.fetch_sub(1); }

    void open() { open_.store(true); }

    void close() {
        open_.store(false);
        while (inFlight_.load() != 0) { std::this_thread::yield(); }
    }

    bool isOpen() const { return open_.load(); }

  private:
    std::atomic<bool> open_{false};
    std::atomic<int> inFlight_{0};
};

constexpr double nanosToMicros(std::int64_t ns) {
    return static_cast<double>(ns) / static_cast<double>(kNanosPerMicro);
}

constexpr double nanosToMillis(std::int64_t ns) {
    return static_cast<double>(ns) / static_cast<double>(kNanosPerMilli);
}

} // namespace dish::bench
