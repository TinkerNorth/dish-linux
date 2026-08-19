// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/RumbleRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::reducer::combinedRumblePlan;
using dish::reducer::isRumbleStop;
using dish::reducer::resolveRumble;
using dish::reducer::RumbleConnectionSnapshot;

namespace {

RumbleConnectionSnapshot conn(const char* id, bool connected, const char* boundDeviceId) {
    RumbleConnectionSnapshot s;
    s.connId = QString::fromUtf8(id);
    s.connected = connected;
    s.boundDeviceId = QString::fromUtf8(boundDeviceId);
    return s;
}

} // namespace

TEST_CASE("resolveRumble routes to the device bound at the matched connection",
          "[rumble][routing]") {
    std::vector<RumbleConnectionSnapshot> snap{
        conn("wifi:a", true, "sdl:1"),
        conn("wifi:b", true, "sdl:2"),
    };
    const auto t = resolveRumble(snap, QStringLiteral("wifi:b"));
    REQUIRE(t.valid());
    REQUIRE(t.deviceId == QStringLiteral("sdl:2"));
}

TEST_CASE("resolveRumble yields None when no connection has the id", "[rumble][routing]") {
    std::vector<RumbleConnectionSnapshot> snap{conn("wifi:a", true, "sdl:1")};
    REQUIRE_FALSE(resolveRumble(snap, QStringLiteral("wifi:missing")).valid());
}

TEST_CASE("resolveRumble yields None for an empty connection id", "[rumble][routing]") {
    std::vector<RumbleConnectionSnapshot> snap{conn("wifi:a", true, "sdl:1")};
    REQUIRE_FALSE(resolveRumble(snap, QString()).valid());
}

TEST_CASE("resolveRumble yields None against an empty snapshot", "[rumble][routing]") {
    REQUIRE_FALSE(resolveRumble({}, QStringLiteral("wifi:a")).valid());
}

TEST_CASE("resolveRumble yields None when the matched connection has nothing bound",
          "[rumble][routing]") {
    std::vector<RumbleConnectionSnapshot> snap{conn("wifi:a", true, "")};
    REQUIRE_FALSE(resolveRumble(snap, QStringLiteral("wifi:a")).valid());
}

TEST_CASE("resolveRumble prefers the connected connection when two share an id",
          "[rumble][routing]") {
    // A stale session must not steal a live controller's rumble.
    std::vector<RumbleConnectionSnapshot> snap{
        conn("wifi:a", false, "sdl:stale"),
        conn("wifi:a", true, "sdl:live"),
    };
    const auto t = resolveRumble(snap, QStringLiteral("wifi:a"));
    REQUIRE(t.valid());
    REQUIRE(t.deviceId == QStringLiteral("sdl:live"));
}

TEST_CASE("resolveRumble falls back to the first match when none are connected",
          "[rumble][routing]") {
    std::vector<RumbleConnectionSnapshot> snap{
        conn("wifi:a", false, "sdl:first"),
        conn("wifi:a", false, "sdl:second"),
    };
    const auto t = resolveRumble(snap, QStringLiteral("wifi:a"));
    REQUIRE(t.valid());
    REQUIRE(t.deviceId == QStringLiteral("sdl:first"));
}

TEST_CASE("resolveRumble ignores connections with a different id", "[rumble][routing]") {
    std::vector<RumbleConnectionSnapshot> snap{
        conn("wifi:x", true, "sdl:x"),
        conn("wifi:y", true, "sdl:y"),
        conn("wifi:z", true, "sdl:z"),
    };
    REQUIRE(resolveRumble(snap, QStringLiteral("wifi:y")).deviceId == QStringLiteral("sdl:y"));
}

TEST_CASE("combinedRumblePlan separates strong and weak across two actuators", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(2, 200, 100);
    REQUIRE(plan.size() == 2U);
    REQUIRE(plan[0] == std::make_pair(0, 200));
    REQUIRE(plan[1] == std::make_pair(1, 100));
}

TEST_CASE("combinedRumblePlan drops a zero-strong actuator on a dual target", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(2, 0, 100);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(1, 100));
}

TEST_CASE("combinedRumblePlan drops a zero-weak actuator on a dual target", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(2, 200, 0);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 200));
}

TEST_CASE("combinedRumblePlan yields nothing when both amplitudes are zero on a dual target",
          "[rumble][plan]") {
    REQUIRE(combinedRumblePlan(2, 0, 0).empty());
}

TEST_CASE("combinedRumblePlan folds a strong-dominant effect onto a single actuator",
          "[rumble][plan]") {
    const auto plan = combinedRumblePlan(1, 200, 50);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 200));
}

TEST_CASE("combinedRumblePlan folds a weak-dominant effect onto a single actuator",
          "[rumble][plan]") {
    const auto plan = combinedRumblePlan(1, 40, 180);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 180));
}

TEST_CASE("combinedRumblePlan drives the single actuator when only weak is set", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(1, 0, 90);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 90));
}

TEST_CASE("combinedRumblePlan yields nothing for a single actuator with no amplitude",
          "[rumble][plan]") {
    REQUIRE(combinedRumblePlan(1, 0, 0).empty());
}

TEST_CASE("combinedRumblePlan yields nothing when there are no actuators", "[rumble][plan]") {
    REQUIRE(combinedRumblePlan(0, 200, 100).empty());
}

TEST_CASE("isRumbleStop is true when both magnitudes are zero or duration is zero",
          "[rumble][stop]") {
    REQUIRE(isRumbleStop(0, 0, 100));
    REQUIRE(isRumbleStop(500, 500, 0));
}

TEST_CASE("isRumbleStop is false when there is a positive magnitude and duration",
          "[rumble][stop]") {
    REQUIRE_FALSE(isRumbleStop(500, 0, 100));
    REQUIRE_FALSE(isRumbleStop(0, 500, 100));
}
