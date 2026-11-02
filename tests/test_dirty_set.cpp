// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/input/DirtySet.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using dish::input::DirtySet;

namespace {

template <std::size_t N> std::vector<std::size_t> drained(DirtySet<N>& set) {
    std::vector<std::size_t> out;
    set.drain([&out](std::size_t slot) { out.push_back(slot); });
    return out;
}

} // namespace

TEST_CASE("a new dirty set is empty and drains nothing", "[dirty-set]") {
    DirtySet<32> set;
    CHECK(set.empty());
    CHECK(drained(set).empty());
}

TEST_CASE("a slot marked twice drains once", "[dirty-set]") {
    DirtySet<32> set;
    CHECK(set.mark(3));
    CHECK(set.mark(3));
    CHECK(set.marked(3));
    CHECK(drained(set) == std::vector<std::size_t>{3});
}

TEST_CASE("the drain visits every marked slot, lowest first, and empties the set", "[dirty-set]") {
    DirtySet<64> set;
    for (const std::size_t slot : {63U, 0U, 17U, 31U, 32U}) { set.mark(slot); }
    CHECK(drained(set) == std::vector<std::size_t>{0, 17, 31, 32, 63});
    CHECK(set.empty());
    CHECK_FALSE(set.marked(17));
    CHECK(drained(set).empty());
}

TEST_CASE("a slot out of range is refused and leaves the set as it was", "[dirty-set]") {
    DirtySet<16> set;
    CHECK_FALSE(set.mark(16));
    CHECK_FALSE(set.mark(1000));
    CHECK(set.empty());
    CHECK_FALSE(set.marked(16));
}

TEST_CASE("a slot marked during the drain waits for the next one", "[dirty-set]") {
    DirtySet<8> set;
    set.mark(1);
    std::vector<std::size_t> first;
    set.drain([&](std::size_t slot) {
        first.push_back(slot);
        set.mark(5);
    });
    CHECK(first == std::vector<std::size_t>{1});
    CHECK(drained(set) == std::vector<std::size_t>{5});
}
