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
// writers. The protected fields are atomics (AtomicWords), so a reader that races a write reads a
// mix of old and new words, never undefined behaviour, and the generation tells it so: each word
// is stored with release and loaded with acquire, so a reader that sees any word of a write also
// sees the odd generation that opened it. No standalone fence orders it: GCC refuses
// atomic_thread_fence under ThreadSanitizer, which cannot see the ordering one gives.
// The satellite's HotSlot and RxPad, as one type.
class Seqlock {
  public:
    // Odd while a write is open. The release stores that follow carry it to any reader that sees
    // one of them.
    void beginWrite() noexcept {
        gen_.store(gen_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    void endWrite() noexcept {
        gen_.store(gen_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // True when everything `load` read belongs to one write. A false read is to be dropped or
    // retried by the caller; the seqlock never waits.
    // `load` reads with acquire only, so the closing load of the generation cannot move ahead of
    // any word it read.
    template <typename Load> bool read(Load&& load) const {
        const std::uint32_t before = gen_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) { return false; }
        load();
        return gen_.load(std::memory_order_relaxed) == before;
    }

    // The generation of the last completed write (or of the open one, odd). A sender that copied
    // state under one generation compares it here after an acquire that synchronised with the
    // writer, to learn whether that state was replaced meanwhile.
    std::uint32_t generation() const noexcept { return gen_.load(std::memory_order_acquire); }

  private:
    std::atomic<std::uint32_t> gen_{0};
};

// A trivially copyable value kept as atomic words, for a Seqlock to guard: stored with release and
// loaded with acquire, which is what the Seqlock's ordering rests on.
template <typename T> class AtomicWords {
    static_assert(std::is_trivially_copyable_v<T>, "copied word by word");

  public:
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / 8;

    void store(const T& value) noexcept {
        std::array<std::uint64_t, kWords> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(raw[i], std::memory_order_release);
        }
    }

    T load() const noexcept {
        std::array<std::uint64_t, kWords> raw{};
        for (std::size_t i = 0; i < kWords; ++i) {
            raw[i] = words_[i].load(std::memory_order_acquire);
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
