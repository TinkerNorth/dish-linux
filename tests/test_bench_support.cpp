// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "bench_support.h"

#include "core/input/Deadzones.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace bench = dish::bench;

namespace {

constexpr std::size_t kSlots = 4096;
constexpr auto kLargestFlatTheSequenceSurvives =
    static_cast<std::int16_t>(bench::kSequenceFloor - 1);
constexpr std::int16_t kBridgeStickFlat = 3277;

std::vector<std::int64_t> oneToHundred() {
    std::vector<std::int64_t> values;
    for (std::int64_t v = 100; v >= 1; --v) { values.push_back(v); }
    return values;
}

} // namespace

TEST_CASE("every sequence survives a stick dead zone below the floor and decodes whole",
          "[bench]") {
    REQUIRE(kBridgeStickFlat < kLargestFlatTheSequenceSurvives);
    std::uint32_t zeroed = 0;
    std::uint32_t garbled = 0;
    for (std::uint32_t s = 0; s < bench::kSequencePeriod; ++s) {
        const std::int16_t axis = bench::encodeSequence(s);
        if (dish::input::deadzone::applyStick(axis, kLargestFlatTheSequenceSurvives) != axis) {
            ++zeroed;
        }
        if (bench::decodeSequence(axis) != s) { ++garbled; }
    }
    CHECK(zeroed == 0U);
    CHECK(garbled == 0U);
}

TEST_CASE("an axis below the floor carries no sequence", "[bench]") {
    CHECK_FALSE(bench::decodeSequence(0).has_value());
    CHECK_FALSE(bench::decodeSequence(kBridgeStickFlat).has_value());
    CHECK_FALSE(bench::decodeSequence(-20000).has_value());
    CHECK(bench::decodeSequence(static_cast<std::int16_t>(bench::kSequenceFloor)) == 0U);
}

TEST_CASE("a frame's sequence wraps at the period", "[bench]") {
    CHECK(bench::sequenceOf(0) == 0U);
    CHECK(bench::sequenceOf(bench::kSequencePeriod - 1) == bench::kSequencePeriod - 1);
    CHECK(bench::sequenceOf(bench::kSequencePeriod) == 0U);
    CHECK(bench::sequenceOf(bench::kSequencePeriod + 5) == 5U);
}

TEST_CASE("a sequence stamp is read in place by peek and only once by take", "[bench]") {
    bench::SequenceStamps<kSlots> stamps;
    CHECK_FALSE(stamps.peek(7).has_value());
    stamps.stamp(7, 1234);
    CHECK(stamps.peek(7) == 1234);
    CHECK(stamps.peek(7) == 1234);
    CHECK_FALSE(stamps.peek(8).has_value());
    CHECK(stamps.take(7) == 1234);
    CHECK_FALSE(stamps.take(7).has_value());
    CHECK_FALSE(stamps.peek(7).has_value());
}

TEST_CASE("a later sequence in the same slot replaces the earlier stamp", "[bench]") {
    bench::SequenceStamps<kSlots> stamps;
    stamps.stamp(3, 10);
    stamps.stamp(3 + kSlots, 20);
    CHECK_FALSE(stamps.peek(3).has_value());
    CHECK(stamps.peek(3 + kSlots) == 20);
    stamps.clear();
    CHECK_FALSE(stamps.peek(3 + kSlots).has_value());
}

TEST_CASE("a percentile is the smallest value with that share at or below it", "[bench]") {
    std::vector<std::int64_t> sorted = oneToHundred();
    std::sort(sorted.begin(), sorted.end());
    CHECK(bench::percentileNs(sorted, bench::kP50) == 50);
    CHECK(bench::percentileNs(sorted, bench::kP99) == 99);
    CHECK(bench::percentileNs(sorted, bench::kP999) == 100);
    CHECK(bench::percentileNs({}, bench::kP50) == 0);
    CHECK(bench::percentileNs({42}, bench::kP999) == 42);
}

TEST_CASE("a summary sorts, takes the percentiles and counts frames over 1 ms", "[bench]") {
    const bench::LatencySummary s = bench::summarize({2'000'000, 1'000, 1'000'000, 1'000'001});
    CHECK(s.frames == 4U);
    CHECK(s.p50Ns == 1'000'000);
    CHECK(s.maxNs == 2'000'000);
    CHECK(s.overOneMs == 2U);
    CHECK(bench::summarize({}).frames == 0U);
}

TEST_CASE("the longest gap is the widest step between sorted arrivals", "[bench]") {
    CHECK(bench::longestGapNs({30, 10, 15, 60}) == 30);
    CHECK(bench::longestGapNs({5}) == 0);
    CHECK(bench::longestGapNs({}) == 0);
}

TEST_CASE("the reported rep is the one whose p99.9 is the median", "[bench]") {
    CHECK(bench::medianRep({30, 10, 20}) == 2U);
    CHECK(bench::medianRep({40, 10, 30, 20}) == 3U);
    CHECK(bench::medianRep({7}) == 0U);
}

TEST_CASE("frame k of a stream is due k periods after the start", "[bench]") {
    CHECK(bench::deadlineOffsetNs(0, 1000.0) == 0);
    CHECK(bench::deadlineOffsetNs(3, 1000.0) == 3'000'000);
    CHECK(bench::deadlineOffsetNs(1, 3.0) == 333'333'333);
    bench::Cadence c(250.0);
    c.advance();
    c.advance();
    CHECK(c.tick() == 2U);
    CHECK(c.dueNs() == 8'000'000);
}

TEST_CASE("the earliest stream is the one whose next frame is due first", "[bench]") {
    std::vector<bench::Cadence> streams{bench::Cadence(1000.0), bench::Cadence(250.0)};
    CHECK(bench::earliest(streams) == 0U);
    streams[0].advance();
    CHECK(bench::earliest(streams) == 1U);
    streams[1].advance();
    for (int i = 0; i < 3; ++i) { streams[0].advance(); }
    CHECK(bench::earliest(streams) == 0U);
}

TEST_CASE("pacing fails on an early frame or a late median and skips a short stream", "[bench]") {
    CHECK(bench::judgePacing(std::vector<std::int64_t>(8, 1'000)).ok);
    std::vector<std::int64_t> early(8, 1'000);
    early[3] = -1;
    const bench::PacingVerdict e = bench::judgePacing(early);
    CHECK(e.early == 1U);
    CHECK_FALSE(e.ok);
    const bench::PacingVerdict late = bench::judgePacing(std::vector<std::int64_t>(8, 2'000'000));
    CHECK(late.medianLatenessNs == 2'000'000);
    CHECK_FALSE(late.ok);
    const bench::PacingVerdict shortStream = bench::judgePacing({-5, -5});
    CHECK_FALSE(shortStream.judged);
    CHECK(shortStream.ok);
}

TEST_CASE("a closed gate refuses a record, and closing waits out one in progress", "[bench]") {
    bench::RecordingGate gate;
    CHECK_FALSE(gate.enter());
    gate.open();
    REQUIRE(gate.enter());
    std::atomic<bool> left{false};
    std::thread recorder([&gate, &left] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        left.store(true);
        gate.leave();
    });
    gate.close();
    const bool leftBeforeClose = left.load();
    recorder.join();
    CHECK(leftBeforeClose);
    CHECK_FALSE(gate.isOpen());
    CHECK_FALSE(gate.enter());
}
