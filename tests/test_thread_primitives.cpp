// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/thread/GraceEpoch.h"
#include "core/thread/LatestValue.h"
#include "core/thread/MpscQueue.h"
#include "core/thread/Seqlock.h"
#include "core/thread/SpscRing.h"
#include "core/thread/WorkerWake.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

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

TEST_CASE("the grace wait returns at once when no bracket is open", "[thread-primitives][grace]") {
    dish::thread::GraceEpoch<4> grace;
    grace.enter(1);
    grace.exit(1);
    const auto started = std::chrono::steady_clock::now();
    grace.waitQuiescent();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(50));
}

TEST_CASE("the grace wait outlasts a bracket that was open when it began",
          "[thread-primitives][grace]") {
    using namespace std::chrono_literals;
    dish::thread::GraceEpoch<4> grace;
    std::atomic<bool> inside{false};
    std::atomic<bool> left{false};
    std::thread reader([&] {
        const dish::thread::GraceGuard bracket(grace, 2);
        inside.store(true);
        std::this_thread::sleep_for(100ms);
        left.store(true);
    });
    while (!inside.load()) { std::this_thread::yield(); }
    grace.waitQuiescent();
    CHECK(left.load());
    reader.join();
}

TEST_CASE("a reader that keeps opening brackets cannot starve the grace wait",
          "[thread-primitives][grace]") {
    dish::thread::GraceEpoch<2> grace;
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const dish::thread::GraceGuard bracket(grace, 0);
        }
    });
    for (int i = 0; i < 1000; ++i) { grace.waitQuiescent(); }
    stop.store(true);
    reader.join();
    SUCCEED("every wait returned");
}

TEST_CASE("a full multi-producer queue refuses the push and counts the drop",
          "[thread-primitives][mpsc]") {
    dish::thread::MpscQueue<int, 4> queue;
    for (int i = 0; i < 4; ++i) {
        CHECK(queue.push([i](int& cell) { cell = i; }));
    }
    CHECK_FALSE(queue.push([](int& cell) { cell = 99; }));
    CHECK(queue.dropped() == 1U);
    int out = -1;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(queue.pop(out));
        CHECK(out == i);
    }
    CHECK(queue.empty());
    CHECK_FALSE(queue.pop(out));
}

TEST_CASE("producers on several threads each keep their own order through the queue",
          "[thread-primitives][mpsc]") {
    constexpr int kProducers = 3;
    constexpr int kEach = 30'000;
    dish::thread::MpscQueue<std::uint64_t, 64> queue;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, p] {
            for (int i = 0; i < kEach;) {
                const auto item =
                    (static_cast<std::uint64_t>(p) << 32) | static_cast<std::uint64_t>(i);
                if (queue.push([item](std::uint64_t& cell) { cell = item; })) { ++i; }
            }
        });
    }
    std::array<std::int64_t, kProducers> last{-1, -1, -1};
    bool ordered = true;
    int popped = 0;
    std::uint64_t item = 0;
    while (popped < kProducers * kEach) {
        if (!queue.pop(item)) { continue; }
        const auto producer = static_cast<std::size_t>(item >> 32);
        const auto sequence = static_cast<std::int64_t>(item & 0xFFFFFFFFU);
        ordered = ordered && sequence == last[producer] + 1;
        last[producer] = sequence;
        ++popped;
    }
    for (auto& p : producers) { p.join(); }
    CHECK(ordered);
}

TEST_CASE("a worker asleep on its wake is woken by the next notify, and none is lost",
          "[thread-primitives][wake]") {
    dish::thread::WorkerWake wake;
    std::atomic<int> posted{0};
    std::atomic<int> handled{0};
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        while (!stop.load()) {
            wake.sleepUnless([&] { return stop.load() || handled.load() < posted.load(); });
            handled.store(posted.load());
        }
    });
    for (int i = 1; i <= 2000; ++i) {
        posted.store(i);
        wake.notify();
        if (i % 100 == 0) {
            while (handled.load() < i) { std::this_thread::yield(); }
        }
    }
    while (handled.load() < 2000) { std::this_thread::yield(); }
    stop.store(true);
    wake.notify();
    worker.join();
    CHECK(handled.load() == 2000);
}

TEST_CASE("a timed sleep with nothing to do ends at its deadline", "[thread-primitives][wake]") {
    dish::thread::WorkerWake wake;
    const auto started = std::chrono::steady_clock::now();
    wake.sleepUnlessUntil([] { return false; }, started + std::chrono::milliseconds(50));
    const auto slept = std::chrono::steady_clock::now() - started;
    // A windows-2022 runner's timed waits end a little early by the steady clock
    // (test_output_worker.cpp met 39.5 ms for 50), so half the deadline still
    // tells a wait from none.
    CHECK(slept >= std::chrono::milliseconds(25));
    CHECK(slept < std::chrono::seconds(5));
}

TEST_CASE("a timed sleep that finds work waiting returns at once", "[thread-primitives][wake]") {
    dish::thread::WorkerWake wake;
    const auto started = std::chrono::steady_clock::now();
    wake.sleepUnlessUntil([] { return true; }, started + std::chrono::seconds(30));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
}

TEST_CASE("a notify ends a timed sleep before its deadline", "[thread-primitives][wake]") {
    dish::thread::WorkerWake wake;
    std::atomic<bool> posted{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&] {
        wake.sleepUnlessUntil([&] { return posted.load(); }, started + std::chrono::seconds(30));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    posted.store(true);
    wake.notify();
    worker.join();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
}

namespace {

// A producer that lands while a sleep is still checking for work: it sees the sleep, flips it and
// owes it a release, the narrowest window a producer can hit.
struct ProducerInsideTheCheck {
    dish::thread::WorkerWake& wake;
    bool operator()() const {
        wake.notify();
        return true;
    }
};

} // namespace

TEST_CASE("a timed sleep takes the release a racing producer owed it",
          "[thread-primitives][wake]") {
    // Left behind, that release would end the next sleep at once instead of at its deadline.
    dish::thread::WorkerWake wake;
    wake.sleepUnlessUntil(ProducerInsideTheCheck{wake},
                          std::chrono::steady_clock::now() + std::chrono::seconds(30));
    const auto started = std::chrono::steady_clock::now();
    wake.sleepUnlessUntil([] { return false; }, started + std::chrono::milliseconds(50));
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(25));
}
