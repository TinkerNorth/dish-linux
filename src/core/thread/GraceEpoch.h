// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace dish::thread {

// The satellite's RxEpoch: lets a cold writer retire something the hot threads may still be
// reading without either side taking a lock. A reader brackets every use of the published thing
// in enter()/exit() on a participant of its own (each participant has exactly one thread writing
// it at a time); the writer swaps the pointer and then waitQuiescent() returns once every bracket
// that might have seen the old one has closed. Dekker's pattern: the reader stores its odd epoch
// and then loads the pointer, the writer stores the pointer and then loads the epochs, and all
// four are seq_cst, so at least one side sees the other. The caller's load and store of the
// pointer must be seq_cst too: GCC refuses the fences that would otherwise order them under
// ThreadSanitizer.
template <std::size_t Participants> class GraceEpoch {
  public:
    static constexpr std::size_t kParticipants = Participants;

    void enter(std::size_t participant) noexcept {
        slots_[participant].epoch.fetch_add(1, std::memory_order_seq_cst);
    }

    void exit(std::size_t participant) noexcept {
        std::atomic<std::uint64_t>& epoch = slots_[participant].epoch;
        epoch.store(epoch.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // Waits for the epoch to change rather than to turn even, so a steady stream of brackets
    // cannot starve it.
    void waitQuiescent() const {
        for (const Slot& slot : slots_) {
            const std::uint64_t seen = slot.epoch.load(std::memory_order_seq_cst);
            if ((seen & 1U) == 0) { continue; }
            while (slot.epoch.load(std::memory_order_acquire) == seen) {
                std::this_thread::yield();
            }
        }
    }

  private:
    // A cache line each, so two readers' brackets never share one.
    static constexpr std::size_t kLineBytes = 64;
    struct Slot {
        std::atomic<std::uint64_t> epoch{0};
        std::array<char, kLineBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
    };

    std::array<Slot, Participants> slots_{};
};

// A bracket on one participant, for the length of a scope.
template <typename Epoch> class GraceGuard {
  public:
    GraceGuard(Epoch& epoch, std::size_t participant) : epoch_(epoch), participant_(participant) {
        epoch_.enter(participant_);
    }
    ~GraceGuard() { epoch_.exit(participant_); }

    GraceGuard(const GraceGuard&) = delete;
    GraceGuard& operator=(const GraceGuard&) = delete;
    GraceGuard(GraceGuard&&) = delete;
    GraceGuard& operator=(GraceGuard&&) = delete;

  private:
    Epoch& epoch_;
    const std::size_t participant_;
};

} // namespace dish::thread
