// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/thread/TrackedMutex.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace dt = dish::thread;

namespace {

#ifdef DISH_LOCK_STATS

namespace lockstats = dish::thread::lockstats;

constexpr auto kHold = std::chrono::milliseconds(100);
constexpr std::uint64_t kHoldFloorNs = 90'000'000;
constexpr std::uint64_t kWaitFloorNs = 50'000'000;
constexpr std::uint64_t kUncontendedCeilingNs = 20'000'000;

lockstats::RoleStats statsFor(const lockstats::Snapshot& snap, const char* lock,
                              dt::ThreadRole role) {
    const lockstats::LockStats* found = snap.find(lock);
    return found != nullptr ? found->role(role) : lockstats::RoleStats{};
}

std::uint64_t histogramTotal(const lockstats::Histogram& histogram) {
    std::uint64_t total = 0;
    for (const std::uint64_t n : histogram) { total += n; }
    return total;
}

std::uint64_t pairCount(const std::vector<lockstats::NamePair>& pairs, const std::string& first,
                        const std::string& second) {
    for (const lockstats::NamePair& pair : pairs) {
        if (pair.first == first && pair.second == second) { return pair.count; }
    }
    return 0;
}

std::size_t pairsNaming(const std::vector<lockstats::NamePair>& pairs, const std::string& prefix) {
    std::size_t named = 0;
    for (const lockstats::NamePair& pair : pairs) {
        if (pair.first.starts_with(prefix) || pair.second.starts_with(prefix)) { ++named; }
    }
    return named;
}

std::uint64_t regionHits(const lockstats::Snapshot& snap, const std::string& region) {
    for (const lockstats::RegionHit& hit : snap.regions) {
        if (hit.region == region) { return hit.hits; }
    }
    return 0;
}

void spinUntil(const std::atomic<bool>& flag) {
    while (!flag.load()) { std::this_thread::yield(); }
}

#else

using namespace std::chrono_literals;

// try_lock on a mutex the calling thread already holds is undefined, so the probe is another
// thread.
bool anotherThreadCanTake(dt::TrackedMutex& m) {
    bool taken = false;
    std::thread probe([&m, &taken] {
        taken = m.try_lock();
        if (taken) { m.unlock(); }
    });
    probe.join();
    return taken;
}

#endif

} // namespace

#ifdef DISH_LOCK_STATS

TEST_CASE("a tracked lock counts every acquisition against the role of the thread that took it",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex m("test.counts");
    {
        const dt::ScopedThreadRole role(dt::ThreadRole::SdlInput);
        for (int i = 0; i < 5; ++i) { const std::lock_guard<dt::TrackedMutex> lk(m); }
        REQUIRE(m.try_lock());
        m.unlock();
        const std::unique_lock<dt::TrackedMutex> deferred(m, std::try_to_lock);
        CHECK(deferred.owns_lock());
    }
    std::thread other([&m] {
        const dt::ScopedThreadRole role(dt::ThreadRole::Gui);
        const std::lock_guard<dt::TrackedMutex> lk(m);
    });
    other.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats input = statsFor(snap, "test.counts", dt::ThreadRole::SdlInput);
    CHECK(input.acquisitions == 7U);
    CHECK(input.contended == 0U);
    CHECK(input.tryFailures == 0U);
    CHECK(histogramTotal(input.waitHistogram) == 7U);
    CHECK(histogramTotal(input.holdHistogram) == 7U);
    CHECK(statsFor(snap, "test.counts", dt::ThreadRole::Gui).acquisitions == 1U);
    CHECK(statsFor(snap, "test.counts", dt::ThreadRole::Other).acquisitions == 0U);
    CHECK(input.touched());
    CHECK_FALSE(statsFor(snap, "test.counts", dt::ThreadRole::Receive).touched());
}

TEST_CASE("a failed try_lock counts as a failure and not as an acquisition",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex m("test.try");
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        const std::lock_guard<dt::TrackedMutex> lk(m);
        held = true;
        spinUntil(release);
    });
    spinUntil(held);
    bool firstTry = true;
    bool secondTry = true;
    {
        const dt::ScopedThreadRole role(dt::ThreadRole::Receive);
        firstTry = m.try_lock();
        const std::unique_lock<dt::TrackedMutex> attempt(m, std::try_to_lock);
        secondTry = attempt.owns_lock();
    }
    release = true;
    holder.join();

    CHECK_FALSE(firstTry);
    CHECK_FALSE(secondTry);
    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats receive = statsFor(snap, "test.try", dt::ThreadRole::Receive);
    CHECK(receive.tryFailures == 2U);
    CHECK(receive.acquisitions == 0U);
    CHECK(receive.touched());
    CHECK(statsFor(snap, "test.try", dt::ThreadRole::Other).acquisitions == 1U);
}

TEST_CASE("under contention the waiter records the wait and the holder records the hold",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex m("test.contention");
    std::atomic<bool> held{false};
    std::atomic<bool> waiterReady{false};
    std::thread holder([&] {
        const dt::ScopedThreadRole role(dt::ThreadRole::Gui);
        const std::lock_guard<dt::TrackedMutex> lk(m);
        held = true;
        spinUntil(waiterReady);
        std::this_thread::sleep_for(kHold);
    });
    spinUntil(held);
    {
        const dt::ScopedThreadRole role(dt::ThreadRole::SdlInput);
        waiterReady = true;
        const std::lock_guard<dt::TrackedMutex> lk(m);
    }
    holder.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats waiter = statsFor(snap, "test.contention", dt::ThreadRole::SdlInput);
    const lockstats::RoleStats owner = statsFor(snap, "test.contention", dt::ThreadRole::Gui);
    CHECK(waiter.acquisitions == 1U);
    CHECK(waiter.contended == 1U);
    CHECK(waiter.waitMaxNs >= kWaitFloorNs);
    CHECK(waiter.waitTotalNs == waiter.waitMaxNs);
    CHECK(waiter.holdMaxNs < kUncontendedCeilingNs);
    CHECK(lockstats::histogramPercentileNs(waiter.waitHistogram, 0.99) >= waiter.waitMaxNs);
    CHECK(owner.acquisitions == 1U);
    CHECK(owner.contended == 0U);
    CHECK(owner.waitMaxNs < kUncontendedCeilingNs);
    CHECK(owner.holdMaxNs >= kHoldFloorNs);
    CHECK(owner.holdTotalNs == owner.holdMaxNs);
}

TEST_CASE("a lock taken under others records a nesting edge from each held lock",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex a("test.nest.a");
    dt::TrackedMutex b("test.nest.b");
    dt::TrackedMutex c("test.nest.c");
    {
        const std::lock_guard<dt::TrackedMutex> la(a);
        const std::lock_guard<dt::TrackedMutex> lb(b);
        const std::lock_guard<dt::TrackedMutex> lc(c);
    }
    bool tried = false;
    {
        const std::lock_guard<dt::TrackedMutex> lc(c);
        const std::unique_lock<dt::TrackedMutex> attempt(a, std::try_to_lock);
        tried = attempt.owns_lock();
    }
    { const std::lock_guard<dt::TrackedMutex> lb(b); }

    CHECK(tried);
    const lockstats::Snapshot snap = lockstats::snapshot();
    CHECK(pairCount(snap.edges, "test.nest.a", "test.nest.b") == 1U);
    CHECK(pairCount(snap.edges, "test.nest.a", "test.nest.c") == 1U);
    CHECK(pairCount(snap.edges, "test.nest.b", "test.nest.c") == 1U);
    CHECK(pairCount(snap.edges, "test.nest.c", "test.nest.a") == 0U);
    CHECK(pairsNaming(snap.edges, "test.nest.") == 3U);
}

TEST_CASE("releasing an outer lock first leaves the inner one on the thread's stack",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex outer("test.order.outer");
    dt::TrackedMutex inner("test.order.inner");
    dt::TrackedMutex later("test.order.later");
    outer.lock();
    inner.lock();
    outer.unlock();
    CHECK(lockstats::heldByThisThread() == std::vector<std::string>{"test.order.inner"});
    later.lock();
    later.unlock();
    inner.unlock();
    CHECK(lockstats::heldByThisThread().empty());

    const lockstats::Snapshot snap = lockstats::snapshot();
    CHECK(pairCount(snap.edges, "test.order.inner", "test.order.later") == 1U);
    CHECK(pairCount(snap.edges, "test.order.outer", "test.order.later") == 0U);
}

TEST_CASE("a blocking region records itself against every lock the thread holds",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex a("test.region.a");
    dt::TrackedMutex b("test.region.b");
    dt::blockingRegion("test.idle");
    {
        const std::lock_guard<dt::TrackedMutex> la(a);
        const std::lock_guard<dt::TrackedMutex> lb(b);
        dt::blockingRegion("test.busy");
    }
    {
        const std::lock_guard<dt::TrackedMutex> la(a);
        dt::blockingRegion("test.busy");
    }

    const lockstats::Snapshot snap = lockstats::snapshot();
    CHECK(regionHits(snap, "test.idle") == 1U);
    CHECK(regionHits(snap, "test.busy") == 2U);
    CHECK(pairCount(snap.blocking, "test.busy", "test.region.a") == 2U);
    CHECK(pairCount(snap.blocking, "test.busy", "test.region.b") == 1U);
    CHECK(pairsNaming(snap.blocking, "test.idle") == 0U);
}

TEST_CASE("a scoped role restores the role that was in effect before it", "[thread][lockstats]") {
    CHECK(dt::currentThreadRole() == dt::ThreadRole::Other);
    {
        const dt::ScopedThreadRole receive(dt::ThreadRole::Receive);
        CHECK(dt::currentThreadRole() == dt::ThreadRole::Receive);
        {
            const dt::ScopedThreadRole playout(dt::ThreadRole::Playout);
            CHECK(dt::currentThreadRole() == dt::ThreadRole::Playout);
        }
        CHECK(dt::currentThreadRole() == dt::ThreadRole::Receive);
    }
    CHECK(dt::currentThreadRole() == dt::ThreadRole::Other);

    lockstats::reset();
    dt::TrackedMutex m("test.role");
    std::thread worker([&m] {
        dt::setThreadRole(dt::ThreadRole::AudioCapture);
        const std::lock_guard<dt::TrackedMutex> lk(m);
    });
    worker.join();
    const lockstats::Snapshot snap = lockstats::snapshot();
    CHECK(statsFor(snap, "test.role", dt::ThreadRole::AudioCapture).acquisitions == 1U);
}

TEST_CASE("a condition_variable_any wait releases the tracked lock and takes it back",
          "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex m("test.cv");
    std::condition_variable_any cv;
    bool ready = false;
    std::atomic<bool> waiting{false};
    std::vector<std::string> heldAfterWait;
    std::thread waiter([&] {
        const dt::ScopedThreadRole role(dt::ThreadRole::Playout);
        std::unique_lock<dt::TrackedMutex> lk(m);
        waiting = true;
        cv.wait(lk, [&ready] { return ready; });
        heldAfterWait = lockstats::heldByThisThread();
    });
    spinUntil(waiting);
    std::this_thread::sleep_for(kHold);
    {
        const std::lock_guard<dt::TrackedMutex> lk(m);
        ready = true;
    }
    cv.notify_all();
    waiter.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats playout = statsFor(snap, "test.cv", dt::ThreadRole::Playout);
    CHECK(playout.acquisitions >= 2U);
    CHECK(playout.holdMaxNs < kHoldFloorNs);
    CHECK(heldAfterWait == std::vector<std::string>{"test.cv"});
}

TEST_CASE("reset clears every count but keeps the registered names", "[thread][lockstats]") {
    dt::TrackedMutex a("test.reset.a");
    dt::TrackedMutex b("test.reset.b");
    {
        const std::lock_guard<dt::TrackedMutex> la(a);
        const std::lock_guard<dt::TrackedMutex> lb(b);
        dt::blockingRegion("test.reset.region");
    }
    lockstats::reset();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::LockStats* kept = snap.find("test.reset.a");
    REQUIRE(kept != nullptr);
    for (const lockstats::RoleStats& role : kept->roles) {
        CHECK(role.acquisitions == 0U);
        CHECK(histogramTotal(role.holdHistogram) == 0U);
    }
    CHECK(snap.edges.empty());
    CHECK(snap.blocking.empty());
    CHECK(snap.regions.empty());
    CHECK(snap.overflows == 0U);
}

TEST_CASE("two tracked locks with one name are counted as one lock", "[thread][lockstats]") {
    lockstats::reset();
    dt::TrackedMutex first("test.shared");
    dt::TrackedMutex second("test.shared");
    { const std::lock_guard<dt::TrackedMutex> lk(first); }
    { const std::lock_guard<dt::TrackedMutex> lk(second); }

    const lockstats::Snapshot snap = lockstats::snapshot();
    CHECK(statsFor(snap, "test.shared", dt::ThreadRole::Other).acquisitions == 2U);
    std::size_t named = 0;
    for (const lockstats::LockStats& lock : snap.locks) {
        if (lock.name == "test.shared") { ++named; }
    }
    CHECK(named == 1U);
}

TEST_CASE("a histogram percentile is the upper edge of the bucket that holds it",
          "[thread][lockstats]") {
    lockstats::Histogram histogram{};
    CHECK(lockstats::histogramPercentileNs(histogram, 0.99) == 0U);
    histogram[10] = 99;
    histogram[20] = 1;
    CHECK(lockstats::histogramPercentileNs(histogram, 0.5) == lockstats::bucketUpperNs(10));
    CHECK(lockstats::histogramPercentileNs(histogram, 0.99) == lockstats::bucketUpperNs(10));
    CHECK(lockstats::histogramPercentileNs(histogram, 0.999) == lockstats::bucketUpperNs(20));
    CHECK(lockstats::bucketUpperNs(10) == 1023U);
    CHECK(lockstats::bucketUpperNs(0) == 0U);
}

#else

static_assert(sizeof(dt::TrackedMutex) == sizeof(std::mutex) + sizeof(const char*),
              "without lock statistics a tracked mutex is a mutex and a name pointer");
static_assert(std::is_empty_v<dt::ScopedThreadRole>,
              "without lock statistics a thread role carries no state");

TEST_CASE("without lock statistics a tracked mutex works with the standard lock types",
          "[thread]") {
    dt::TrackedMutex m("test.plain");
    CHECK(std::string(m.name()) == "test.plain");
    {
        const std::lock_guard<dt::TrackedMutex> lk(m);
        CHECK_FALSE(anotherThreadCanTake(m));
    }
    CHECK(anotherThreadCanTake(m));
    std::unique_lock<dt::TrackedMutex> lk(m, std::try_to_lock);
    CHECK(lk.owns_lock());
    std::condition_variable_any cv;
    CHECK_FALSE(cv.wait_for(lk, 1ms, [] { return false; }));
    CHECK(lk.owns_lock());
}

TEST_CASE("without lock statistics roles and blocking regions leave the lock working", "[thread]") {
    dt::TrackedMutex m("test.plain.region");
    const dt::ScopedThreadRole role(dt::ThreadRole::Receive);
    dt::setThreadRole(dt::ThreadRole::Gui);
    {
        const std::lock_guard<dt::TrackedMutex> lk(m);
        dt::blockingRegion("test.plain.region");
        CHECK_FALSE(anotherThreadCanTake(m));
    }
    CHECK(anotherThreadCanTake(m));
}

#endif

TEST_CASE("every thread role has its own name", "[thread]") {
    std::set<std::string> names;
    for (std::size_t r = 0; r < dt::kThreadRoleCount; ++r) {
        names.insert(dt::threadRoleName(static_cast<dt::ThreadRole>(r)));
    }
    CHECK(names.size() == dt::kThreadRoleCount);
    CHECK(std::string(dt::threadRoleName(dt::ThreadRole::SdlInput)) == "SdlInput");
}
