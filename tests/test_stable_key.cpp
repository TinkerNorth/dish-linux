// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/model/IdentityKey.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using dish::model::stableKey;

TEST_CASE("stableKey prefers the machineId", "[stablekey]") {
    CHECK(stableKey("abc123", "192.168.1.5", 9876) == "mid:abc123");
}

TEST_CASE("stableKey falls back to ip:port without a machineId", "[stablekey]") {
    CHECK(stableKey("", "192.168.1.5", 9876) == "192.168.1.5:9876");
}

TEST_CASE("stableKey is the same for one machineId at different IPs", "[stablekey]") {
    CHECK(stableKey("m", "10.0.0.5", 9876) == stableKey("m", "10.0.0.99", 9876));
}

TEST_CASE("stableKey treats a blank machineId as absent", "[stablekey]") {
    CHECK(stableKey("   ", "10.0.0.1", 9876) == "10.0.0.1:9876");
}
