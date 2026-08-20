// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/TouchpadModeResolve.h"

#include <catch2/catch_test_macros.hpp>

namespace proto = dish::proto;
namespace reducer = dish::reducer;

namespace {
const std::string kOff{proto::touchpadModeName(proto::kTouchpadModeOff)};
const std::string kDs4{proto::touchpadModeName(proto::kTouchpadModeDs4)};
const std::string kMouse{proto::touchpadModeName(proto::kTouchpadModeMouse)};
} // namespace

TEST_CASE("resolve: ds4 pick with a touch source and an offering type -> ds4",
          "[touchpad-mode][resolve]") {
    REQUIRE(reducer::resolveTouchpadMode(kDs4, true, true, false) == proto::kTouchpadModeDs4);
}

TEST_CASE("resolve: ds4 pick without a touch source -> off (never mouse)",
          "[touchpad-mode][resolve]") {
    // A blocked pick declares off; it never crosses over to the other routing.
    REQUIRE(reducer::resolveTouchpadMode(kDs4, false, true, true) == proto::kTouchpadModeOff);
}

TEST_CASE("resolve: ds4 pick when the type gates the pad render off -> off",
          "[touchpad-mode][resolve]") {
    REQUIRE(reducer::resolveTouchpadMode(kDs4, true, false, true) == proto::kTouchpadModeOff);
}

TEST_CASE("resolve: mouse pick needs a touch source AND the host grant",
          "[touchpad-mode][resolve]") {
    REQUIRE(reducer::resolveTouchpadMode(kMouse, true, false, true) == proto::kTouchpadModeMouse);
    REQUIRE(reducer::resolveTouchpadMode(kMouse, true, true, false) == proto::kTouchpadModeOff);
    REQUIRE(reducer::resolveTouchpadMode(kMouse, false, true, true) == proto::kTouchpadModeOff);
}

TEST_CASE("resolve: mouse never falls back to ds4 when blocked", "[touchpad-mode][resolve]") {
    REQUIRE(reducer::resolveTouchpadMode(kMouse, true, true, false) == proto::kTouchpadModeOff);
}

TEST_CASE("resolve: an off or never-made pick is off regardless of capability",
          "[touchpad-mode][resolve]") {
    REQUIRE(reducer::resolveTouchpadMode(kOff, true, true, true) == proto::kTouchpadModeOff);
    REQUIRE(reducer::resolveTouchpadMode("", true, true, true) == proto::kTouchpadModeOff);
}

TEST_CASE("resolve: an unknown pick string collapses to off", "[touchpad-mode][resolve]") {
    REQUIRE(reducer::resolveTouchpadMode("banana", true, true, true) == proto::kTouchpadModeOff);
}

TEST_CASE("typeOffersDs4Touchpad: supported + ds4 mode offers", "[touchpad-mode][gate]") {
    REQUIRE(reducer::typeOffersDs4Touchpad(true, {"ds4"}));
    REQUIRE(reducer::typeOffersDs4Touchpad(true, {"mouse", "ds4"}));
}

TEST_CASE("typeOffersDs4Touchpad: unsupported never offers", "[touchpad-mode][gate]") {
    REQUIRE_FALSE(reducer::typeOffersDs4Touchpad(false, {"ds4"}));
    REQUIRE_FALSE(reducer::typeOffersDs4Touchpad(false, {}));
}

TEST_CASE("typeOffersDs4Touchpad: absent modes is a pre-modes catalog (offers)",
          "[touchpad-mode][gate]") {
    // Back-compat: a catalog with no modes array predates the gate, so a
    // supported touchpad feature is still assumed pad-capable.
    REQUIRE(reducer::typeOffersDs4Touchpad(true, {}));
}

TEST_CASE("typeOffersDs4Touchpad: a modes list without ds4 gates off", "[touchpad-mode][gate]") {
    REQUIRE_FALSE(reducer::typeOffersDs4Touchpad(true, {"mouse"}));
}

TEST_CASE("isValidTouchpadModeName accepts exactly the three wire strings",
          "[touchpad-mode][names]") {
    REQUIRE(reducer::isValidTouchpadModeName(kOff));
    REQUIRE(reducer::isValidTouchpadModeName(kDs4));
    REQUIRE(reducer::isValidTouchpadModeName(kMouse));
    REQUIRE_FALSE(reducer::isValidTouchpadModeName(""));
    REQUIRE_FALSE(reducer::isValidTouchpadModeName("DS4"));
    REQUIRE_FALSE(reducer::isValidTouchpadModeName("touchpad"));
}

TEST_CASE("kTouchpadModeNames is the canonical off/ds4/mouse order", "[touchpad-mode][names]") {
    REQUIRE(reducer::kTouchpadModeNames.size() == 3);
    REQUIRE(reducer::kTouchpadModeNames[0] == kOff);
    REQUIRE(reducer::kTouchpadModeNames[1] == kDs4);
    REQUIRE(reducer::kTouchpadModeNames[2] == kMouse);
}
