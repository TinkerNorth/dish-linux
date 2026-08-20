// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The SDL bridge is never constructed here: it initialises SDL and hangs in a
// unit test. Each case asserts the remap mutation, then cross-checks it through
// mapJoystick so a routed source really drives the matching output.

#include "Input/JoystickMapping.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::input::captureAxisPasses;
using dish::input::captureButtonPasses;
using dish::input::captureHatPasses;
using dish::input::CaptureKind;
using dish::input::InvertTarget;
using dish::input::JoystickRemap;
using dish::input::JoystickSnapshot;
using dish::input::mapJoystick;
using dish::input::RemapButton;
using dish::input::RemapTarget;
using dish::input::TriggerSourceKind;
using dish::input::withAssignment;
using dish::input::withInvert;
using B = dish::input::GamepadInputProcessor::Buttons;
namespace hat = dish::input::hat;

namespace {

int kindOf(CaptureKind k) { return static_cast<int>(k); }
int btn(const JoystickRemap& r, RemapButton b) { return r.buttons[static_cast<int>(b)]; }

// The caller-owned arrays must outlive every read of the snapshot.
JoystickSnapshot makeSnap(std::int16_t* axes, int axisCount, bool* buttons, int buttonCount,
                          std::uint8_t* hats, int hatCount) {
    JoystickSnapshot s{};
    s.axes = axes;
    s.axisCount = axisCount;
    s.buttons = buttons;
    s.buttonCount = buttonCount;
    s.hats = hats;
    s.hatCount = hatCount;
    return s;
}

} // namespace

TEST_CASE("withAssignment routes a face button to its raw source", "[remap][assign]") {
    JoystickRemap r = withAssignment(JoystickRemap{}, RemapTarget::A, kindOf(CaptureKind::Button),
                                     /*index=*/9);
    REQUIRE(btn(r, RemapButton::A) == 9);
    // 1 is B's default source: an untouched target keeps its routing.
    REQUIRE(btn(r, RemapButton::B) == 1);

    std::array<bool, 12> buttons{};
    buttons[9] = true;
    auto snap = makeSnap(nullptr, 0, buttons.data(), static_cast<int>(buttons.size()), nullptr, 0);
    const auto st = mapJoystick(snap, r);
    REQUIRE((st.wButtons & B::kA) != 0);
    REQUIRE((st.wButtons & B::kB) == 0);
}

TEST_CASE("withAssignment routes each shoulder / system / thumb button", "[remap][assign]") {
    JoystickRemap r{};
    r = withAssignment(r, RemapTarget::LeftShoulder, kindOf(CaptureKind::Button), 3);
    r = withAssignment(r, RemapTarget::RightShoulder, kindOf(CaptureKind::Button), 4);
    r = withAssignment(r, RemapTarget::Back, kindOf(CaptureKind::Button), 5);
    r = withAssignment(r, RemapTarget::Start, kindOf(CaptureKind::Button), 6);
    r = withAssignment(r, RemapTarget::LeftThumb, kindOf(CaptureKind::Button), 7);
    r = withAssignment(r, RemapTarget::RightThumb, kindOf(CaptureKind::Button), 8);
    REQUIRE(btn(r, RemapButton::LeftShoulder) == 3);
    REQUIRE(btn(r, RemapButton::RightShoulder) == 4);
    REQUIRE(btn(r, RemapButton::Back) == 5);
    REQUIRE(btn(r, RemapButton::Start) == 6);
    REQUIRE(btn(r, RemapButton::LeftThumb) == 7);
    REQUIRE(btn(r, RemapButton::RightThumb) == 8);
}

TEST_CASE("withAssignment routes a stick axis and clears the right-stick adaptive flag",
          "[remap][assign]") {
    JoystickRemap r = withAssignment(JoystickRemap{}, RemapTarget::LeftStickX,
                                     kindOf(CaptureKind::Axis), /*index=*/4);
    REQUIRE(r.leftStickX == 4);
    REQUIRE(r.useAdaptiveRightStick == true);

    JoystickRemap rr =
        withAssignment(JoystickRemap{}, RemapTarget::RightStickX, kindOf(CaptureKind::Axis), 5);
    REQUIRE(rr.rightStickX == 5);
    // An explicit right-stick choice opts out of the < 6-axis adaptive fallback.
    REQUIRE(rr.useAdaptiveRightStick == false);

    JoystickRemap ry =
        withAssignment(JoystickRemap{}, RemapTarget::RightStickY, kindOf(CaptureKind::Axis), 2);
    REQUIRE(ry.rightStickY == 2);
    REQUIRE(ry.useAdaptiveRightStick == false);
}

TEST_CASE("withAssignment tags a trigger source by capture kind", "[remap][assign]") {
    JoystickRemap a =
        withAssignment(JoystickRemap{}, RemapTarget::LeftTrigger, kindOf(CaptureKind::Axis), 5);
    REQUIRE(a.leftTrigger.kind == TriggerSourceKind::Axis);
    REQUIRE(a.leftTrigger.index == 5);
    REQUIRE(a.useAdaptiveTriggers == false);

    // A button-kind trigger is digital: full scale while pressed.
    JoystickRemap b =
        withAssignment(JoystickRemap{}, RemapTarget::RightTrigger, kindOf(CaptureKind::Button), 8);
    REQUIRE(b.rightTrigger.kind == TriggerSourceKind::Button);
    REQUIRE(b.rightTrigger.index == 8);
    REQUIRE(b.useAdaptiveTriggers == false);

    std::array<std::int16_t, 6> axes{};
    std::array<bool, 12> buttons{};
    buttons[8] = true;
    auto snap = makeSnap(axes.data(), static_cast<int>(axes.size()), buttons.data(),
                         static_cast<int>(buttons.size()), nullptr, 0);
    const auto st = mapJoystick(snap, b);
    REQUIRE(st.rt == 255);
}

TEST_CASE("withAssignment dpad via hat vs via button", "[remap][assign]") {
    // A hat-kind capture points the dpad at a hat index and clears any button
    // override, which is why DpadUp reads back as -1.
    JoystickRemap viaHat =
        withAssignment(JoystickRemap{}, RemapTarget::DpadUp, kindOf(CaptureKind::Hat), /*index=*/0);
    REQUIRE(viaHat.hatIndex == 0);
    REQUIRE(btn(viaHat, RemapButton::DpadUp) == -1);

    std::array<std::uint8_t, 1> hats{hat::kUp};
    auto hatSnap = makeSnap(nullptr, 0, nullptr, 0, hats.data(), 1);
    const auto hatSt = mapJoystick(hatSnap, viaHat);
    REQUIRE((hatSt.wButtons & B::kDpadUp) != 0);

    JoystickRemap viaBtn =
        withAssignment(JoystickRemap{}, RemapTarget::DpadDown, kindOf(CaptureKind::Button), 11);
    REQUIRE(btn(viaBtn, RemapButton::DpadDown) == 11);
    // The hat still drives the other directions.
    REQUIRE(viaBtn.hatIndex == 0);

    std::array<bool, 12> buttons{};
    buttons[11] = true;
    auto btnSnap =
        makeSnap(nullptr, 0, buttons.data(), static_cast<int>(buttons.size()), nullptr, 0);
    const auto btnSt = mapJoystick(btnSnap, viaBtn);
    REQUIRE((btnSt.wButtons & B::kDpadDown) != 0);
}

TEST_CASE("withInvert toggles each Y-invert flag", "[remap][assign]") {
    JoystickRemap base{}; // defaults: both inverts true
    REQUIRE(base.invertLeftY == true);
    REQUIRE(base.invertRightY == true);

    JoystickRemap offL = withInvert(base, InvertTarget::LeftY, false);
    REQUIRE(offL.invertLeftY == false);
    REQUIRE(offL.invertRightY == true); // unrelated flag untouched

    JoystickRemap offR = withInvert(base, InvertTarget::RightY, false);
    REQUIRE(offR.invertRightY == false);
    REQUIRE(offR.invertLeftY == true);

    JoystickRemap onAgain = withInvert(offL, InvertTarget::LeftY, true);
    REQUIRE(onAgain.invertLeftY == true);

    std::array<std::int16_t, 2> axes{0, 1000};
    auto snap = makeSnap(axes.data(), 2, nullptr, 0, nullptr, 0);
    REQUIRE(mapJoystick(snap, base).ly == -1000); // default: inverted
    REQUIRE(mapJoystick(snap, offL).ly == 1000);  // invert off: passthrough
}

TEST_CASE("captureAxisPasses gates a deliberate move and rejects jitter", "[remap][capture]") {
    REQUIRE_FALSE(captureAxisPasses(0));
    REQUIRE_FALSE(captureAxisPasses(3000));
    REQUIRE_FALSE(captureAxisPasses(-3000));
    REQUIRE(captureAxisPasses(20000));
    REQUIRE(captureAxisPasses(-20000));
    REQUIRE(captureAxisPasses(32767));
    REQUIRE(captureAxisPasses(-32768));
}

TEST_CASE("button and hat captures pass on a real press / non-centered direction",
          "[remap][capture]") {
    REQUIRE(captureButtonPasses());
    REQUIRE_FALSE(captureHatPasses(hat::kCentered));
    REQUIRE(captureHatPasses(hat::kUp));
    REQUIRE(captureHatPasses(hat::kLeft));
    REQUIRE(captureHatPasses(hat::kUp | hat::kRight)); // diagonal still passes
}
