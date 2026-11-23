// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The feedback state a bench test borrows, and how it goes back
// (docs/diagnostics-bundle.md, "The bench", rules 2 and 3). The light bar, the
// player LEDs, the adaptive triggers and the mic lamp are state: a pad shows
// the last value it was sent until the next one. So each pad's ledger keeps
// the last value its owner sent for each surface (the game through its host,
// and this client for the lamp it lights on a mute), a test holds the surface
// it drives, a value sent while the test holds it is remembered rather than
// written, and the test's end writes the remembered value back, or the idle
// value for a surface nothing ever set.

#pragma once

#include "core/model/Protocol.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

namespace dish::reducer {

enum class BenchSurface : std::uint8_t { Lightbar, PlayerLeds, TriggerEffects, MicLamp };
inline constexpr std::size_t kBenchSurfaces = 4;

using TriggerBlock = std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>;

struct LightbarValue {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const LightbarValue&) const = default;
};

// The two DualSense effect blocks in the wire's order, left then right.
struct TriggerEffectsValue {
    TriggerBlock left{};
    TriggerBlock right{};

    bool operator==(const TriggerEffectsValue&) const = default;
};

// One value for each surface.
struct SurfaceValues {
    LightbarValue lightbar{};
    std::uint8_t playerLeds = 0;
    TriggerEffectsValue triggerEffects{};
    std::uint8_t micLamp = 0;

    bool operator==(const SurfaceValues&) const = default;
};

// What a surface nothing ever set goes back to: the bar dark, no player LED lit, no trigger
// resistance (effect mode 0) and the lamp off. This client never sets a surface of its own
// accord, so nothing it could put back knows better.
inline constexpr LightbarValue kIdleLightbar{0x00, 0x00, 0x00};
inline constexpr std::uint8_t kIdlePlayerLeds = 0x00;
inline constexpr TriggerEffectsValue kIdleTriggerEffects{};
inline constexpr std::uint8_t kIdleMicLamp = proto::kMicLedStateOff;
inline constexpr SurfaceValues kIdleSurfaces{kIdleLightbar, kIdlePlayerLeds, kIdleTriggerEffects,
                                             kIdleMicLamp};

inline constexpr std::uint8_t surfaceBit(BenchSurface surface) {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(surface));
}

// The owner's last value for each surface, and which surfaces it ever set.
struct RememberedSurfaces {
    SurfaceValues values{};
    std::uint8_t setMask = 0;

    bool operator==(const RememberedSurfaces&) const = default;
};

inline bool ownerSet(const RememberedSurfaces& remembered, BenchSurface surface) {
    return (remembered.setMask & surfaceBit(surface)) != 0;
}

inline RememberedSurfaces withLightbar(RememberedSurfaces remembered, LightbarValue value) {
    remembered.values.lightbar = value;
    remembered.setMask |= surfaceBit(BenchSurface::Lightbar);
    return remembered;
}

inline RememberedSurfaces withPlayerLeds(RememberedSurfaces remembered, std::uint8_t ledMask) {
    remembered.values.playerLeds = ledMask;
    remembered.setMask |= surfaceBit(BenchSurface::PlayerLeds);
    return remembered;
}

inline RememberedSurfaces withTriggerEffects(RememberedSurfaces remembered,
                                             const TriggerEffectsValue& value) {
    remembered.values.triggerEffects = value;
    remembered.setMask |= surfaceBit(BenchSurface::TriggerEffects);
    return remembered;
}

inline RememberedSurfaces withMicLamp(RememberedSurfaces remembered, std::uint8_t state) {
    remembered.values.micLamp = state;
    remembered.setMask |= surfaceBit(BenchSurface::MicLamp);
    return remembered;
}

// One write of a state surface: a test's step, or what goes back when the test ends. Only the
// value of the surface it names is written.
struct SurfaceWrite {
    BenchSurface surface = BenchSurface::Lightbar;
    SurfaceValues values{};

    bool operator==(const SurfaceWrite&) const = default;
};

// Rule 2: the owner's last value, or the idle one for a surface the owner never set.
inline SurfaceWrite restoreWriteFor(BenchSurface surface, const RememberedSurfaces& remembered) {
    const bool set = ownerSet(remembered, surface);
    return SurfaceWrite{surface, set ? remembered.values : kIdleSurfaces};
}

// Rule 3: a value sent for a surface a test holds is remembered, not written. The other surfaces
// keep following their owner.
inline bool writesThrough(std::uint8_t heldMask, BenchSurface surface) {
    return (heldMask & surfaceBit(surface)) == 0;
}

// One pad's ledger, shared by every thread that routes the pad's feedback (a session's receive
// thread, a Moonlight control thread, the GUI for the mute lamp) and the GUI's bench. No
// TrackedMutex and no allocation: a sender takes turns with the other senders on a flag held for
// a few stores, as OutputMailbox's producers do (a spin that yields only when two senders meet,
// the producers' turn flag CONTRIBUTING.md allows), and a reader copies the values out under a
// generation without taking a turn. Every generation and
// hold operation is sequentially consistent, which is what giveBack leans on: a sender that found
// its surface held stored its value before the test released it, so the snapshot after the
// release sees that value.
class SurfaceLedger {
  public:
    struct Snapshot {
        RememberedSurfaces surfaces{};
        std::uint32_t generation = 0;
    };

    // A sender, on its own thread: remembers the value and answers whether to write it now.
    bool rememberLightbar(LightbarValue value) noexcept {
        return rememberWith(BenchSurface::Lightbar, &withLightbar, value);
    }
    bool rememberPlayerLeds(std::uint8_t ledMask) noexcept {
        return rememberWith(BenchSurface::PlayerLeds, &withPlayerLeds, ledMask);
    }
    bool rememberTriggerEffects(const TriggerEffectsValue& value) noexcept {
        return rememberWith(BenchSurface::TriggerEffects, &withTriggerEffects, value);
    }
    bool rememberMicLamp(std::uint8_t state) noexcept {
        return rememberWith(BenchSurface::MicLamp, &withMicLamp, state);
    }

    // The bench: a test takes the surface, and gives it back.
    void hold(BenchSurface surface) noexcept {
        held_.fetch_or(surfaceBit(surface), std::memory_order_seq_cst);
    }
    void release(BenchSurface surface) noexcept {
        held_.fetch_and(static_cast<std::uint8_t>(~surfaceBit(surface)), std::memory_order_seq_cst);
    }
    bool holds(BenchSurface surface) const noexcept {
        return !writesThrough(held_.load(std::memory_order_seq_cst), surface);
    }

    // One whole write's values and its generation. A reader takes no turn on the senders' flag,
    // so it never holds a sender up; it waits out a write in progress, a few stores long. A
    // sender waits only for another sender's turn (lockWriters).
    Snapshot snapshot() const noexcept {
        for (;;) {
            const std::uint32_t before = generation_.load(std::memory_order_seq_cst);
            const bool writeOpen = (before & 1U) != 0;
            if (!writeOpen) {
                const RememberedSurfaces surfaces = load();
                const bool unchanged = generation_.load(std::memory_order_seq_cst) == before;
                if (unchanged) { return Snapshot{surfaces, before}; }
            }
            std::this_thread::yield();
        }
    }

    std::uint32_t generation() const noexcept {
        return generation_.load(std::memory_order_seq_cst);
    }

  private:
    static constexpr std::size_t kWordBytes = sizeof(std::uint64_t);
    static constexpr std::size_t kWords =
        (sizeof(RememberedSurfaces) + kWordBytes - 1) / kWordBytes;

    template <typename Edit, typename Value>
    bool rememberWith(BenchSurface surface, Edit edit, const Value& value) noexcept {
        lockWriters();
        return publish(surface, edit(load(), value));
    }

    void lockWriters() noexcept {
        std::uint32_t free = 0;
        while (!writer_.compare_exchange_weak(free, 1, std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
            free = 0;
            std::this_thread::yield();
        }
    }

    // Under the writers' flag: the values, then whether the surface is held, read after the
    // closing generation so a release that comes later finds this write.
    bool publish(BenchSurface surface, const RememberedSurfaces& next) noexcept {
        generation_.store(generation_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_seq_cst);
        store(next);
        generation_.store(generation_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_seq_cst);
        writer_.store(0, std::memory_order_release);
        return writesThrough(held_.load(std::memory_order_seq_cst), surface);
    }

    void store(const RememberedSurfaces& value) noexcept {
        std::array<std::uint64_t, kWords> raw{};
        std::memcpy(raw.data(), &value, sizeof(value));
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(raw[i], std::memory_order_seq_cst);
        }
    }

    RememberedSurfaces load() const noexcept {
        std::array<std::uint64_t, kWords> raw{};
        for (std::size_t i = 0; i < kWords; ++i) {
            raw[i] = words_[i].load(std::memory_order_seq_cst);
        }
        // Through bytes, as thread::AtomicWords copies: GCC warns when an array of another type
        // is copied into a class whose members have default initialisers.
        const auto bytes = std::bit_cast<std::array<unsigned char, sizeof(raw)>>(raw);
        RememberedSurfaces value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    }

    std::atomic<std::uint32_t> writer_{0};
    std::atomic<std::uint32_t> generation_{0};
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
    std::atomic<std::uint8_t> held_{0};
};

// How many times a test's end writes its surface back before it trusts the owner's next message
// instead: each further time means a sender remembered a newer value during the write before.
inline constexpr int kGiveBackAttempts = 8;

// The test's end (rules 2 and 3): the surface goes back to its owner, and its value is written
// until no sender remembered a newer one meanwhile, so the pad's last write is the owner's last
// value. A sender that found the surface released writes its own value itself.
template <typename Write>
void giveBack(SurfaceLedger& ledger, BenchSurface surface, Write&& write) {
    ledger.release(surface);
    for (int attempt = 0; attempt < kGiveBackAttempts; ++attempt) {
        const SurfaceLedger::Snapshot snapshot = ledger.snapshot();
        write(restoreWriteFor(surface, snapshot.surfaces));
        const bool noNewerValue = ledger.generation() == snapshot.generation;
        if (noNewerValue) { return; }
    }
}

} // namespace dish::reducer
