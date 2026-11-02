// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#ifdef DISH_LOCK_STATS
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#endif

namespace dish::thread {

enum class ThreadRole : std::uint8_t {
    Gui,
    SdlInput,
    UsbReader,
    Receive,
    Heartbeat,
    Playout,
    AudioCapture,
    MoonlightRx,
    Update,
    Other,
};

inline constexpr std::size_t kThreadRoleCount = static_cast<std::size_t>(ThreadRole::Other) + 1;

constexpr const char* threadRoleName(ThreadRole role) {
    switch (role) {
    case ThreadRole::Gui:
        return "Gui";
    case ThreadRole::SdlInput:
        return "SdlInput";
    case ThreadRole::UsbReader:
        return "UsbReader";
    case ThreadRole::Receive:
        return "Receive";
    case ThreadRole::Heartbeat:
        return "Heartbeat";
    case ThreadRole::Playout:
        return "Playout";
    case ThreadRole::AudioCapture:
        return "AudioCapture";
    case ThreadRole::MoonlightRx:
        return "MoonlightRx";
    case ThreadRole::Update:
        return "Update";
    case ThreadRole::Other:
        return "Other";
    }
    return "Other";
}

#ifndef DISH_LOCK_STATS

class TrackedMutex {
  public:
    explicit TrackedMutex(const char* name) noexcept : name_(name) {}
    TrackedMutex(const TrackedMutex&) = delete;
    TrackedMutex& operator=(const TrackedMutex&) = delete;

    void lock() { mutex_.lock(); }
    bool try_lock() { return mutex_.try_lock(); }
    void unlock() { mutex_.unlock(); }

    const char* name() const noexcept { return name_; }

  private:
    std::mutex mutex_;
    const char* name_;
};

class ScopedThreadRole {
  public:
    explicit ScopedThreadRole(ThreadRole) noexcept {}
    ~ScopedThreadRole() {}
    ScopedThreadRole(const ScopedThreadRole&) = delete;
    ScopedThreadRole& operator=(const ScopedThreadRole&) = delete;
};

inline void setThreadRole(ThreadRole) noexcept {}
inline void blockingRegion(const char*) noexcept {}

#else

namespace lockstats {

inline constexpr std::size_t kMaxLocks = 64;
inline constexpr std::size_t kMaxRegions = 32;
inline constexpr std::size_t kMaxHeld = 16;
inline constexpr std::size_t kPairSlots = 256;
inline constexpr std::size_t kHistogramBuckets = 40;

using Clock = std::chrono::steady_clock;
using Histogram = std::array<std::uint64_t, kHistogramBuckets>;

namespace detail {

struct Counters {
    std::atomic<std::uint64_t> acquisitions{0};
    std::atomic<std::uint64_t> contended{0};
    std::atomic<std::uint64_t> tryFailures{0};
    std::atomic<std::uint64_t> waitTotalNs{0};
    std::atomic<std::uint64_t> waitMaxNs{0};
    std::atomic<std::uint64_t> holdTotalNs{0};
    std::atomic<std::uint64_t> holdMaxNs{0};
    std::array<std::atomic<std::uint64_t>, kHistogramBuckets> waitHistogram{};
    std::array<std::atomic<std::uint64_t>, kHistogramBuckets> holdHistogram{};
};

struct LockSlot {
    std::atomic<const char*> name{nullptr};
    std::array<Counters, kThreadRoleCount> roles{};
};

struct PairTable {
    std::array<std::atomic<std::uint32_t>, kPairSlots> keys{};
    std::array<std::atomic<std::uint64_t>, kPairSlots> counts{};
};

struct Registry {
    std::atomic<bool> naming{false};
    std::atomic<std::size_t> lockCount{0};
    std::array<LockSlot, kMaxLocks> locks{};
    std::atomic<std::size_t> regionCount{0};
    std::array<std::atomic<const char*>, kMaxRegions> regions{};
    std::array<std::atomic<std::uint64_t>, kMaxRegions> regionHits{};
    PairTable edges{};
    PairTable blocking{};
    std::atomic<std::uint64_t> overflows{0};
};

inline Registry registry;

inline constexpr int kNoId = -1;

struct Held {
    int lock = kNoId;
    ThreadRole role = ThreadRole::Other;
    Clock::time_point since{};
};

struct ThreadState {
    ThreadRole role = ThreadRole::Other;
    std::size_t depth = 0;
    std::array<Held, kMaxHeld> held{};
};

inline thread_local ThreadState threadState;

inline std::uint64_t nanosBetween(Clock::time_point from, Clock::time_point to) {
    const auto span = std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
    return span > 0 ? static_cast<std::uint64_t>(span) : 0;
}

inline std::size_t bucketFor(std::uint64_t ns) {
    const auto bucket = static_cast<std::size_t>(std::bit_width(ns));
    return bucket < kHistogramBuckets ? bucket : kHistogramBuckets - 1;
}

inline void raiseMax(std::atomic<std::uint64_t>& max, std::uint64_t value) {
    std::uint64_t seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

inline void bump(std::atomic<std::uint64_t>& counter, std::uint64_t amount = 1) {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

class NamingGuard {
  public:
    NamingGuard() {
        while (registry.naming.exchange(true, std::memory_order_acquire)) {}
    }
    ~NamingGuard() { registry.naming.store(false, std::memory_order_release); }
    NamingGuard(const NamingGuard&) = delete;
    NamingGuard& operator=(const NamingGuard&) = delete;
};

inline bool sameName(const char* a, const char* b) { return a == b || std::strcmp(a, b) == 0; }

template <typename NameAt> int findName(const char* name, std::size_t published, NameAt nameAt) {
    for (std::size_t i = 0; i < published; ++i) {
        if (sameName(nameAt(i), name)) { return static_cast<int>(i); }
    }
    return kNoId;
}

template <typename NameAt, typename Publish>
int internName(const char* name, std::atomic<std::size_t>& entries, std::size_t capacity,
               NameAt nameAt, Publish publish) {
    const int known = findName(name, entries.load(std::memory_order_acquire), nameAt);
    if (known != kNoId) { return known; }
    const NamingGuard guard;
    const std::size_t published = entries.load(std::memory_order_acquire);
    const int raced = findName(name, published, nameAt);
    if (raced != kNoId) { return raced; }
    if (published >= capacity) {
        bump(registry.overflows);
        return kNoId;
    }
    publish(published, name);
    entries.store(published + 1, std::memory_order_release);
    return static_cast<int>(published);
}

inline int internLock(const char* name) {
    return internName(
        name, registry.lockCount, kMaxLocks,
        [](std::size_t i) { return registry.locks[i].name.load(std::memory_order_acquire); },
        [](std::size_t i, const char* n) {
            registry.locks[i].name.store(n, std::memory_order_release);
        });
}

inline int internRegion(const char* name) {
    return internName(
        name, registry.regionCount, kMaxRegions,
        [](std::size_t i) { return registry.regions[i].load(std::memory_order_acquire); },
        [](std::size_t i, const char* n) {
            registry.regions[i].store(n, std::memory_order_release);
        });
}

inline int lockId(const char* name, std::atomic<int>& cache) {
    const int cached = cache.load(std::memory_order_acquire);
    if (cached != kNoId) { return cached; }
    const int id = internLock(name);
    if (id != kNoId) { cache.store(id, std::memory_order_release); }
    return id;
}

inline std::uint32_t pairKey(int first, int second) {
    return (static_cast<std::uint32_t>(first + 1) << 16) | static_cast<std::uint32_t>(second + 1);
}

inline void countPair(PairTable& table, std::uint32_t key) {
    std::size_t slot = (static_cast<std::size_t>(key) * 2654435761U) % kPairSlots;
    for (std::size_t probe = 0; probe < kPairSlots; ++probe, slot = (slot + 1) % kPairSlots) {
        std::uint32_t seen = table.keys[slot].load(std::memory_order_acquire);
        if (seen == 0 &&
            table.keys[slot].compare_exchange_strong(seen, key, std::memory_order_acq_rel)) {
            seen = key;
        }
        if (seen != key) { continue; }
        bump(table.counts[slot]);
        return;
    }
    bump(registry.overflows);
}

inline Counters& countersFor(int lock, ThreadRole role) {
    return registry.locks[static_cast<std::size_t>(lock)].roles[static_cast<std::size_t>(role)];
}

inline void noteBlockingAttempt(int lock) {
    if (lock == kNoId) { return; }
    const ThreadState& state = threadState;
    for (std::size_t i = 0; i < state.depth; ++i) {
        const int outer = state.held[i].lock;
        if (outer != kNoId) { countPair(registry.edges, pairKey(outer, lock)); }
    }
}

inline void noteAcquired(int lock, Clock::time_point requested, Clock::time_point acquired,
                         bool contended) {
    if (lock == kNoId) { return; }
    ThreadState& state = threadState;
    Counters& counters = countersFor(lock, state.role);
    const std::uint64_t waited = nanosBetween(requested, acquired);
    bump(counters.acquisitions);
    if (contended) { bump(counters.contended); }
    bump(counters.waitTotalNs, waited);
    raiseMax(counters.waitMaxNs, waited);
    bump(counters.waitHistogram[bucketFor(waited)]);
    if (state.depth >= kMaxHeld) {
        bump(registry.overflows);
        return;
    }
    state.held[state.depth] = Held{lock, state.role, acquired};
    ++state.depth;
}

inline void noteTryFailed(int lock) {
    if (lock == kNoId) { return; }
    bump(countersFor(lock, threadState.role).tryFailures);
}

inline void noteReleased(int lock, Clock::time_point released) {
    if (lock == kNoId) { return; }
    ThreadState& state = threadState;
    for (std::size_t i = state.depth; i-- > 0;) {
        if (state.held[i].lock != lock) { continue; }
        const Held held = state.held[i];
        for (std::size_t j = i + 1; j < state.depth; ++j) { state.held[j - 1] = state.held[j]; }
        --state.depth;
        Counters& counters = countersFor(lock, held.role);
        const std::uint64_t heldFor = nanosBetween(held.since, released);
        bump(counters.holdTotalNs, heldFor);
        raiseMax(counters.holdMaxNs, heldFor);
        bump(counters.holdHistogram[bucketFor(heldFor)]);
        return;
    }
}

} // namespace detail

struct RoleStats {
    std::uint64_t acquisitions = 0;
    std::uint64_t contended = 0;
    std::uint64_t tryFailures = 0;
    std::uint64_t waitTotalNs = 0;
    std::uint64_t waitMaxNs = 0;
    std::uint64_t holdTotalNs = 0;
    std::uint64_t holdMaxNs = 0;
    Histogram waitHistogram{};
    Histogram holdHistogram{};

    bool touched() const { return acquisitions != 0 || tryFailures != 0; }
};

struct LockStats {
    std::string name;
    std::array<RoleStats, kThreadRoleCount> roles{};

    const RoleStats& role(ThreadRole r) const { return roles[static_cast<std::size_t>(r)]; }
};

struct NamePair {
    std::string first;
    std::string second;
    std::uint64_t count = 0;
};

struct RegionHit {
    std::string region;
    std::uint64_t hits = 0;
};

struct Snapshot {
    std::vector<LockStats> locks;
    std::vector<NamePair> edges;
    std::vector<NamePair> blocking;
    std::vector<RegionHit> regions;
    std::uint64_t overflows = 0;

    const LockStats* find(const std::string& name) const {
        for (const LockStats& lock : locks) {
            if (lock.name == name) { return &lock; }
        }
        return nullptr;
    }
};

inline std::uint64_t bucketUpperNs(std::size_t bucket) {
    if (bucket == 0) { return 0; }
    if (bucket >= 64) { return UINT64_MAX; }
    return (std::uint64_t{1} << bucket) - 1;
}

inline std::uint64_t histogramPercentileNs(const Histogram& histogram, double fraction) {
    std::uint64_t total = 0;
    for (const std::uint64_t n : histogram) { total += n; }
    if (total == 0) { return 0; }
    const double wanted = fraction * static_cast<double>(total);
    auto rank = static_cast<std::uint64_t>(wanted);
    if (static_cast<double>(rank) < wanted) { ++rank; }
    if (rank == 0) { rank = 1; }
    std::uint64_t seen = 0;
    for (std::size_t bucket = 0; bucket < kHistogramBuckets; ++bucket) {
        seen += histogram[bucket];
        if (seen >= rank) { return bucketUpperNs(bucket); }
    }
    return bucketUpperNs(kHistogramBuckets - 1);
}

inline RoleStats readCounters(const detail::Counters& counters) {
    RoleStats out;
    out.acquisitions = counters.acquisitions.load(std::memory_order_relaxed);
    out.contended = counters.contended.load(std::memory_order_relaxed);
    out.tryFailures = counters.tryFailures.load(std::memory_order_relaxed);
    out.waitTotalNs = counters.waitTotalNs.load(std::memory_order_relaxed);
    out.waitMaxNs = counters.waitMaxNs.load(std::memory_order_relaxed);
    out.holdTotalNs = counters.holdTotalNs.load(std::memory_order_relaxed);
    out.holdMaxNs = counters.holdMaxNs.load(std::memory_order_relaxed);
    for (std::size_t b = 0; b < kHistogramBuckets; ++b) {
        out.waitHistogram[b] = counters.waitHistogram[b].load(std::memory_order_relaxed);
        out.holdHistogram[b] = counters.holdHistogram[b].load(std::memory_order_relaxed);
    }
    return out;
}

inline std::string lockName(int id) {
    const char* name = detail::registry.locks[static_cast<std::size_t>(id)].name.load();
    return name != nullptr ? name : "";
}

inline std::string regionName(int id) {
    const char* name = detail::registry.regions[static_cast<std::size_t>(id)].load();
    return name != nullptr ? name : "";
}

template <typename NameOf>
std::vector<NamePair> readPairs(const detail::PairTable& table, NameOf firstName) {
    std::vector<NamePair> out;
    for (std::size_t slot = 0; slot < kPairSlots; ++slot) {
        const std::uint32_t key = table.keys[slot].load(std::memory_order_acquire);
        const std::uint64_t hits = table.counts[slot].load(std::memory_order_relaxed);
        if (key == 0 || hits == 0) { continue; }
        const int first = static_cast<int>(key >> 16) - 1;
        const int second = static_cast<int>(key & 0xFFFFU) - 1;
        out.push_back(NamePair{firstName(first), lockName(second), hits});
    }
    return out;
}

inline Snapshot snapshot() {
    const detail::Registry& reg = detail::registry;
    Snapshot snap;
    const std::size_t lockCount = reg.lockCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < lockCount; ++i) {
        LockStats lock;
        lock.name = lockName(static_cast<int>(i));
        for (std::size_t r = 0; r < kThreadRoleCount; ++r) {
            lock.roles[r] = readCounters(reg.locks[i].roles[r]);
        }
        snap.locks.push_back(std::move(lock));
    }
    snap.edges = readPairs(reg.edges, lockName);
    snap.blocking = readPairs(reg.blocking, regionName);
    const std::size_t regionCount = reg.regionCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < regionCount; ++i) {
        const std::uint64_t hits = reg.regionHits[i].load(std::memory_order_relaxed);
        if (hits != 0) { snap.regions.push_back(RegionHit{regionName(static_cast<int>(i)), hits}); }
    }
    snap.overflows = reg.overflows.load(std::memory_order_relaxed);
    return snap;
}

inline void clearCounters(detail::Counters& counters) {
    counters.acquisitions.store(0, std::memory_order_relaxed);
    counters.contended.store(0, std::memory_order_relaxed);
    counters.tryFailures.store(0, std::memory_order_relaxed);
    counters.waitTotalNs.store(0, std::memory_order_relaxed);
    counters.waitMaxNs.store(0, std::memory_order_relaxed);
    counters.holdTotalNs.store(0, std::memory_order_relaxed);
    counters.holdMaxNs.store(0, std::memory_order_relaxed);
    for (auto& n : counters.waitHistogram) { n.store(0, std::memory_order_relaxed); }
    for (auto& n : counters.holdHistogram) { n.store(0, std::memory_order_relaxed); }
}

inline void reset() {
    detail::Registry& reg = detail::registry;
    for (auto& lock : reg.locks) {
        for (auto& role : lock.roles) { clearCounters(role); }
    }
    for (auto& hits : reg.regionHits) { hits.store(0, std::memory_order_relaxed); }
    for (auto& n : reg.edges.counts) { n.store(0, std::memory_order_relaxed); }
    for (auto& n : reg.blocking.counts) { n.store(0, std::memory_order_relaxed); }
    reg.overflows.store(0, std::memory_order_relaxed);
}

inline std::vector<std::string> heldByThisThread() {
    std::vector<std::string> names;
    const detail::ThreadState& state = detail::threadState;
    for (std::size_t i = 0; i < state.depth; ++i) {
        if (state.held[i].lock != detail::kNoId) { names.push_back(lockName(state.held[i].lock)); }
    }
    return names;
}

} // namespace lockstats

inline void setThreadRole(ThreadRole role) noexcept { lockstats::detail::threadState.role = role; }

inline ThreadRole currentThreadRole() noexcept { return lockstats::detail::threadState.role; }

class ScopedThreadRole {
  public:
    explicit ScopedThreadRole(ThreadRole role) noexcept : previous_(currentThreadRole()) {
        setThreadRole(role);
    }
    ~ScopedThreadRole() { setThreadRole(previous_); }
    ScopedThreadRole(const ScopedThreadRole&) = delete;
    ScopedThreadRole& operator=(const ScopedThreadRole&) = delete;

  private:
    ThreadRole previous_;
};

inline void blockingRegion(const char* what) noexcept {
    namespace d = lockstats::detail;
    const int region = d::internRegion(what);
    if (region == d::kNoId) { return; }
    d::bump(d::registry.regionHits[static_cast<std::size_t>(region)]);
    const d::ThreadState& state = d::threadState;
    for (std::size_t i = 0; i < state.depth; ++i) {
        const int held = state.held[i].lock;
        if (held != d::kNoId) { d::countPair(d::registry.blocking, d::pairKey(region, held)); }
    }
}

class TrackedMutex {
  public:
    explicit TrackedMutex(const char* name) noexcept : name_(name) {}
    TrackedMutex(const TrackedMutex&) = delete;
    TrackedMutex& operator=(const TrackedMutex&) = delete;

    void lock() {
        namespace d = lockstats::detail;
        const int id = d::lockId(name_, id_);
        d::noteBlockingAttempt(id);
        const auto requested = lockstats::Clock::now();
        bool contended = false;
        if (!mutex_.try_lock()) {
            contended = true;
            mutex_.lock();
        }
        d::noteAcquired(id, requested, lockstats::Clock::now(), contended);
    }

    bool try_lock() {
        namespace d = lockstats::detail;
        const int id = d::lockId(name_, id_);
        if (!mutex_.try_lock()) {
            d::noteTryFailed(id);
            return false;
        }
        const auto acquired = lockstats::Clock::now();
        d::noteAcquired(id, acquired, acquired, false);
        return true;
    }

    void unlock() {
        lockstats::detail::noteReleased(id_.load(std::memory_order_acquire),
                                        lockstats::Clock::now());
        mutex_.unlock();
    }

    const char* name() const noexcept { return name_; }

  private:
    std::mutex mutex_;
    const char* name_;
    std::atomic<int> id_{lockstats::detail::kNoId};
};

#endif

} // namespace dish::thread
