// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace dish::input {

// The pads a pump cycle saw move, by slot: marking one twice is marking it once, and the drain
// visits each marked slot once, lowest first, and leaves the set empty.
template <std::size_t Slots> class DirtySet {
    static_assert(Slots > 0 && Slots <= 64, "one word of bits");

  public:
    static constexpr std::size_t kSlots = Slots;

    // False for a slot out of range, which is left unmarked.
    bool mark(std::size_t slot) {
        if (slot >= Slots) { return false; }
        bits_ |= std::uint64_t{1} << slot;
        return true;
    }

    bool marked(std::size_t slot) const {
        return slot < Slots && (bits_ & (std::uint64_t{1} << slot)) != 0;
    }

    bool empty() const { return bits_ == 0; }

    template <typename Visit> void drain(Visit&& visit) {
        std::uint64_t pending = bits_;
        bits_ = 0;
        while (pending != 0) {
            const auto slot = static_cast<std::size_t>(std::countr_zero(pending));
            pending &= pending - 1;
            visit(slot);
        }
    }

  private:
    std::uint64_t bits_ = 0;
};

} // namespace dish::input
