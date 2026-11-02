// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dish::thread {

// A sequence lock for state one writer at a time changes and other threads copy out: the writer
// is either the state's single owner thread or a cold path holding the lock that serialises its
// writers. The protected fields are relaxed atomics (AtomicWords), so a reader that races a write
// reads a mix of old and new words, never undefined behaviour, and the generation tells it so.
// The satellite's HotSlot and RxPad, as one type.
class Seqlock {
  public:
    // Odd while a write is open. The release fence keeps the stores that follow from becoming
    // visible ahead of the odd generation.
    void beginWrite() noexcept {
        gen_.store(gen_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void endWrite() noexcept {
        gen_.store(gen_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // True when everything `load` read belongs to one write. A false read is to be dropped or
    // retried by the caller; the seqlock never waits.
    template <typename Load> bool read(Load&& load) const {
        const std::uint32_t before = gen_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) { return false; }
        load();
        std::atomic_thread_fence(std::memory_order_acquire);
        return gen_.load(std::memory_order_relaxed) == before;
    }

    // The generation of the last completed write (or of the open one, odd). A sender that copied
    // state under one generation compares it here after an acquire that synchronised with the
    // writer, to learn whether that state was replaced meanwhile.
    std::uint32_t generation() const noexcept { return gen_.load(std::memory_order_acquire); }

  private:
    std::atomic<std::uint32_t> gen_{0};
};

// A trivially copyable value kept as relaxed atomic words, for a Seqlock to guard.
template <typename T> class AtomicWords {
    static_assert(std::is_trivially_copyable_v<T>, "copied word by word");

  public:
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / 8;

    void store(const T& value) noexcept {
        std::array<std::uint64_t, kWords> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(raw[i], std::memory_order_relaxed);
        }
    }

    T load() const noexcept {
        std::array<std::uint64_t, kWords> raw{};
        for (std::size_t i = 0; i < kWords; ++i) {
            raw[i] = words_[i].load(std::memory_order_relaxed);
        }
        // Through bytes: GCC warns when an array of another type is copied into a class whose
        // members have default initialisers.
        const auto bytes = std::bit_cast<std::array<unsigned char, sizeof(raw)>>(raw);
        T value{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }

  private:
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

} // namespace dish::thread
