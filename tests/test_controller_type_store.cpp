// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "StateSourceProbe.h"
#include "source/store/ControllerTypeStore.h"

#include <catch2/catch_test_macros.hpp>

using dish::source::ControllerTypeMap;
using dish::source::ControllerTypeStore;

namespace {
constexpr int kXbox = 0;
constexpr int kPlayStation = 1;
} // namespace

TEST_CASE("ControllerTypeStore: setType then typeFor round-trips the value", "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kPlayStation);
    REQUIRE(store.typeFor("conn-1", "slot-A") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: typeFor an unset connection-slot is null", "[typestore]") {
    ControllerTypeStore store;
    REQUIRE_FALSE(store.typeFor("conn-1", "slot-A").has_value());
}

TEST_CASE("ControllerTypeStore: setType overwrites an existing value", "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);
    store.setType("conn-1", "slot-A", kPlayStation);
    REQUIRE(store.typeFor("conn-1", "slot-A") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: setTypeIfAbsent writes when the key is unset", "[typestore]") {
    ControllerTypeStore store;
    store.setTypeIfAbsent("conn-1", "slot-A", kPlayStation);
    REQUIRE(store.typeFor("conn-1", "slot-A") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: setTypeIfAbsent keeps the existing value when present",
          "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kPlayStation);
    store.setTypeIfAbsent("conn-1", "slot-A", kXbox);
    REQUIRE(store.typeFor("conn-1", "slot-A") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: same slot id under different connections does not collide",
          "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);
    store.setType("conn-2", "slot-A", kPlayStation);
    REQUIRE(store.typeFor("conn-1", "slot-A") == kXbox);
    REQUIRE(store.typeFor("conn-2", "slot-A") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: slotTypesFor returns only bound slots that carry a type",
          "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);
    store.setType("conn-1", "slot-B", kPlayStation);
    store.setType("conn-2", "slot-A", kPlayStation);

    const auto types = store.slotTypesFor("conn-1", {"slot-A", "slot-B", "slot-unbound"});

    REQUIRE(types.size() == 2);
    REQUIRE(types.at("slot-A") == kXbox);
    REQUIRE(types.at("slot-B") == kPlayStation);
    REQUIRE(types.find("slot-unbound") == types.end());
}

TEST_CASE("ControllerTypeStore: clear removes only the exact connection-slot entry",
          "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);
    store.setType("conn-1", "slot-B", kPlayStation);

    store.clear("conn-1", "slot-A");

    REQUIRE_FALSE(store.typeFor("conn-1", "slot-A").has_value());
    REQUIRE(store.typeFor("conn-1", "slot-B") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: clearConnection drops every slot for a connection", "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);
    store.setType("conn-1", "slot-B", kPlayStation);
    store.setType("conn-2", "slot-A", kPlayStation);

    store.clearConnection("conn-1");

    REQUIRE_FALSE(store.typeFor("conn-1", "slot-A").has_value());
    REQUIRE_FALSE(store.typeFor("conn-1", "slot-B").has_value());
    REQUIRE(store.typeFor("conn-2", "slot-A") == kPlayStation);
}

TEST_CASE("ControllerTypeStore: clear of an unset connection-slot is a no-op", "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);
    store.clear("conn-1", "slot-missing");
    REQUIRE(store.typeFor("conn-1", "slot-A") == kXbox);
    REQUIRE(store.state().value().size() == 1U);
}

TEST_CASE("ControllerTypeStore: a no-op setTypeIfAbsent/clear emits nothing", "[typestore]") {
    ControllerTypeStore store;
    store.setType("conn-1", "slot-A", kXbox);

    dish::test::StateSourceProbe<ControllerTypeMap> probe(store.state());
    REQUIRE(probe.count() == 1U); // eager initial only

    // Key present / key absent: neither mutates the map.
    store.setTypeIfAbsent("conn-1", "slot-A", kPlayStation);
    store.clear("conn-1", "slot-missing");
    REQUIRE(probe.count() == 1U);

    store.setType("conn-1", "slot-B", kPlayStation);
    REQUIRE(probe.count() == 2U);
}
