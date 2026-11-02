// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/thread/LatestValue.h"
#include "core/thread/Seqlock.h"
#include "core/thread/SpscRing.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

using dish::thread::AtomicWords;
using dish::thread::LatestValue;
using dish::thread::Seqlock;
using dish::thread::SpscRing;

namespace {

// Five equal words: a read that mixes two writes shows unequal ones.
struct Stamp {
    std::array<std::uint32_t, 5> words{};
    bool whole() const {
        for (const std::uint32_t w : words) {
            if (w != words[0]) { return false; }
        }
        return true;
    }
};

struct Counted {
    explicit Counted(int v, std::atomic<int>* live) : value(v), live_(live) { live_->fetch_add(1); }
    ~Counted() { live_->fetch_sub(1); }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    Counted(Counted&&) = delete;
    Counted& operator=(Counted&&) = delete;
    int value;

  private:
    std::atomic<int>* live_;
};

} // namespace

TEST_CASE("atomic words round-trip a value of an odd size", "[thread-primitives][seqlock]") {
    struct Odd {
        std::array<std::uint8_t, 13> bytes{};
    };
    Odd in;
    for (std::uint8_t i = 0; i < in.bytes.size(); ++i) {
        in.bytes[i] = static_cast<std::uint8_t>(i * 7);
    }
    AtomicWords<Odd> words;
    words.store(in);
    CHECK(words.load().bytes == in.bytes);
    CHECK(AtomicWords<Odd>::kWords == 2);
}

TEST_CASE("a seqlock read during an open write fails, and after it succeeds",
          "[thread-primitives][seqlock]") {
    Seqlock seq;
    AtomicWords<Stamp> value;
    seq.beginWrite();
    value.store(Stamp{{1, 1, 1, 1, 1}});
    Stamp seen;
    CHECK_FALSE(seq.read([&] { seen = value.load(); }));
    seq.endWrite();
    CHECK(seq.read([&] { seen = value.load(); }));
    CHECK(seen.words[0] == 1);
    CHECK(seq.generation() == 2);
}

TEST_CASE("a seqlock reader never accepts a torn value from a writer on another thread",
          "[thread-primitives][seqlock]") {
    Seqlock seq;
    AtomicWords<Stamp> value;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::uint32_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
            seq.beginWrite();
            value.store(Stamp{{n, n, n, n, n}});
            seq.endWrite();
        }
    });
    int accepted = 0;
    int torn = 0;
    std::uint32_t last = 0;
    bool ordered = true;
    for (int i = 0; i < 200'000; ++i) {
        Stamp seen;
        if (!seq.read([&] { seen = value.load(); })) { continue; }
        ++accepted;
        if (!seen.whole()) { ++torn; }
        if (seen.words[0] < last) { ordered = false; }
        last = seen.words[0];
    }
    stop.store(true);
    writer.join();
    CHECK(accepted > 0);
    CHECK(torn == 0);
    CHECK(ordered);
}

TEST_CASE("the latest value hands over only the newest and frees what it replaced",
          "[thread-primitives][latest-value]") {
    std::atomic<int> live{0};
    {
        LatestValue<Counted> box;
        CHECK_FALSE(box.take());
        box.publish(std::make_unique<Counted>(1, &live));
        box.publish(std::make_unique<Counted>(2, &live));
        CHECK(live.load() == 1);
        const auto got = box.take();
        REQUIRE(got);
        CHECK(got->value == 2);
        CHECK_FALSE(box.take());
        box.publish(std::make_unique<Counted>(3, &live));
    }
    CHECK(live.load() == 0);
}

TEST_CASE("a consumer of the latest value sees values in publish order, ending with the last",
          "[thread-primitives][latest-value]") {
    constexpr int kValues = 20'000;
    std::atomic<int> live{0};
    LatestValue<Counted> box;
    std::thread producer([&] {
        for (int v = 1; v <= kValues; ++v) { box.publish(std::make_unique<Counted>(v, &live)); }
    });
    int last = 0;
    bool increasing = true;
    while (last != kValues) {
        if (const auto got = box.take()) {
            if (got->value <= last) { increasing = false; }
            last = got->value;
        }
    }
    producer.join();
    CHECK(increasing);
    CHECK(live.load() == 0);
}

TEST_CASE("a full ring refuses the push and a drained one hands items back in order",
          "[thread-primitives][spsc]") {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) { CHECK(ring.push(i)); }
    CHECK_FALSE(ring.push(99));
    for (int i = 0; i < 4; ++i) {
        int out = -1;
        CHECK(ring.pop(out));
        CHECK(out == i);
    }
    int none = -1;
    CHECK_FALSE(ring.pop(none));
}

TEST_CASE("a ring carries a producer thread's items to a consumer thread in order",
          "[thread-primitives][spsc]") {
    constexpr std::uint64_t kItems = 200'000;
    SpscRing<std::uint64_t, 64> ring;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems;) {
            if (ring.push(i)) { ++i; }
        }
    });
    std::uint64_t expected = 0;
    bool ordered = true;
    while (expected < kItems) {
        std::uint64_t out = 0;
        if (!ring.pop(out)) { continue; }
        if (out != expected) { ordered = false; }
        ++expected;
    }
    producer.join();
    CHECK(ordered);
}
