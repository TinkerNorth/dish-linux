// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "JoystickMapping.h"

#include <cstddef>

namespace dish::input {

namespace {

// RemapButton is a dense enum over JoystickRemap::buttons; the cast is the
// array index, not a value conversion.
constexpr std::size_t idx(RemapButton b) { return static_cast<std::size_t>(b); }

} // namespace

std::int16_t axisAt(const JoystickSnapshot& snap, int index) {
    if (snap.axes == nullptr || index < 0 || index >= snap.axisCount) { return 0; }
    return snap.axes[index];
}

bool buttonAt(const JoystickSnapshot& snap, int index) {
    if (snap.buttons == nullptr || index < 0 || index >= snap.buttonCount) { return false; }
    return snap.buttons[index];
}

std::uint8_t triggerFromAxis(std::int16_t v) {
    if (v <= 0) { return 0; }
    return static_cast<std::uint8_t>((static_cast<int>(v) * 255) / 32767);
}

bool hasTriggerAxes(const JoystickSnapshot& snap) { return snap.axisCount >= 6; }

namespace {

void applyButton(std::uint16_t& btn, std::uint16_t bit, const JoystickSnapshot& snap, int source) {
    if (source >= 0 && buttonAt(snap, source)) { btn |= bit; }
}

// A Button source is full-scale on press; an unassigned (-1) source reads 0.
std::uint8_t triggerValue(const JoystickSnapshot& snap, const TriggerSource& src) {
    if (src.index < 0) { return 0; }
    if (src.kind == TriggerSourceKind::Axis) { return triggerFromAxis(axisAt(snap, src.index)); }
    return buttonAt(snap, src.index) ? 255 : 0;
}

} // namespace

namespace {

// ~half the int16 range, well above the ~10 % stick-flat noise floor, so a
// resting axis (even a trigger resting at the negative extreme) never
// self-assigns. Duplicated rather than referencing the bridge's default flat,
// to keep this TU SDL-free.
constexpr int kCaptureAxisThreshold = 16000;

} // namespace

bool captureAxisPasses(int value) {
    const int mag = value < 0 ? -value : value;
    return mag > kCaptureAxisThreshold;
}

bool captureButtonPasses() { return true; }

bool captureHatPasses(int hatValue) { return (hatValue & 0xFF) != hat::kCentered; }

JoystickRemap withAssignment(JoystickRemap base, RemapTarget target, int kind, int index) {
    const auto setButton = [&](RemapButton b) { base.buttons[idx(b)] = index; };
    // A Hat capture points the dpad at that hat and drops the direction's
    // button override so the hat wins; a Button capture leaves hatIndex alone,
    // so the other directions keep reading the hat.
    const auto setDpad = [&](RemapButton b) {
        if (kind == static_cast<int>(CaptureKind::Hat)) {
            base.hatIndex = index;
            base.buttons[idx(b)] = -1;
        } else {
            base.buttons[idx(b)] = index;
        }
    };
    // Either way the explicit choice disables the adaptive fallback.
    const auto setTrigger = [&](TriggerSource& t) {
        t.kind = (kind == static_cast<int>(CaptureKind::Button)) ? TriggerSourceKind::Button
                                                                 : TriggerSourceKind::Axis;
        t.index = index;
        base.useAdaptiveTriggers = false;
    };

    switch (target) {
    case RemapTarget::A:
        setButton(RemapButton::A);
        break;
    case RemapTarget::B:
        setButton(RemapButton::B);
        break;
    case RemapTarget::X:
        setButton(RemapButton::X);
        break;
    case RemapTarget::Y:
        setButton(RemapButton::Y);
        break;
    case RemapTarget::DpadUp:
        setDpad(RemapButton::DpadUp);
        break;
    case RemapTarget::DpadDown:
        setDpad(RemapButton::DpadDown);
        break;
    case RemapTarget::DpadLeft:
        setDpad(RemapButton::DpadLeft);
        break;
    case RemapTarget::DpadRight:
        setDpad(RemapButton::DpadRight);
        break;
    case RemapTarget::LeftShoulder:
        setButton(RemapButton::LeftShoulder);
        break;
    case RemapTarget::RightShoulder:
        setButton(RemapButton::RightShoulder);
        break;
    case RemapTarget::Back:
        setButton(RemapButton::Back);
        break;
    case RemapTarget::Start:
        setButton(RemapButton::Start);
        break;
    case RemapTarget::LeftThumb:
        setButton(RemapButton::LeftThumb);
        break;
    case RemapTarget::RightThumb:
        setButton(RemapButton::RightThumb);
        break;
    case RemapTarget::LeftStickX:
        base.leftStickX = index;
        break;
    case RemapTarget::LeftStickY:
        base.leftStickY = index;
        break;
    case RemapTarget::RightStickX:
        base.rightStickX = index;
        base.useAdaptiveRightStick = false;
        break;
    case RemapTarget::RightStickY:
        base.rightStickY = index;
        base.useAdaptiveRightStick = false;
        break;
    case RemapTarget::LeftTrigger:
        setTrigger(base.leftTrigger);
        break;
    case RemapTarget::RightTrigger:
        setTrigger(base.rightTrigger);
        break;
    }
    return base;
}

JoystickRemap withInvert(JoystickRemap base, InvertTarget which, bool on) {
    switch (which) {
    case InvertTarget::LeftY:
        base.invertLeftY = on;
        break;
    case InvertTarget::RightY:
        base.invertRightY = on;
        break;
    }
    return base;
}

GamepadInputProcessor::DeviceState mapJoystick(const JoystickSnapshot& snap,
                                               const JoystickRemap& remap) {
    using B = GamepadInputProcessor::Buttons;
    GamepadInputProcessor::DeviceState st{};

    const bool fewAxes = !hasTriggerAxes(snap);

    // ── Sticks ──────────────────────────────────────────────────────────────
    st.lx = remap.leftStickX >= 0 ? axisAt(snap, remap.leftStickX) : 0;
    {
        const std::int16_t ly = remap.leftStickY >= 0 ? axisAt(snap, remap.leftStickY) : 0;
        st.ly = remap.invertLeftY ? static_cast<std::int16_t>(-ly) : ly;
    }
    int rightX = remap.rightStickX;
    int rightY = remap.rightStickY;
    if (remap.useAdaptiveRightStick && fewAxes) {
        rightX = 2;
        rightY = 3;
    }
    st.rx = rightX >= 0 ? axisAt(snap, rightX) : 0;
    {
        const std::int16_t ry = rightY >= 0 ? axisAt(snap, rightY) : 0;
        st.ry = remap.invertRightY ? static_cast<std::int16_t>(-ry) : ry;
    }

    // ── Buttons ─────────────────────────────────────────────────────────────
    std::uint16_t btn = 0;
    const auto src = [&](RemapButton b) { return remap.buttons[idx(b)]; };
    applyButton(btn, B::kDpadUp, snap, src(RemapButton::DpadUp));
    applyButton(btn, B::kDpadDown, snap, src(RemapButton::DpadDown));
    applyButton(btn, B::kDpadLeft, snap, src(RemapButton::DpadLeft));
    applyButton(btn, B::kDpadRight, snap, src(RemapButton::DpadRight));
    applyButton(btn, B::kStart, snap, src(RemapButton::Start));
    applyButton(btn, B::kBack, snap, src(RemapButton::Back));
    applyButton(btn, B::kLeftThumb, snap, src(RemapButton::LeftThumb));
    applyButton(btn, B::kRightThumb, snap, src(RemapButton::RightThumb));
    applyButton(btn, B::kLeftShoulder, snap, src(RemapButton::LeftShoulder));
    applyButton(btn, B::kRightShoulder, snap, src(RemapButton::RightShoulder));
    applyButton(btn, B::kA, snap, src(RemapButton::A));
    applyButton(btn, B::kB, snap, src(RemapButton::B));
    applyButton(btn, B::kX, snap, src(RemapButton::X));
    applyButton(btn, B::kY, snap, src(RemapButton::Y));

    // ── Triggers ────────────────────────────────────────────────────────────
    if (remap.useAdaptiveTriggers && fewAxes) {
        // A generic < 6-axis pad cannot be told statically whether buttons 8/9
        // are L2/R2 or something else; the per-device remap resolves it.
        st.lt = buttonAt(snap, 8) ? 255 : 0;
        st.rt = buttonAt(snap, 9) ? 255 : 0;
    } else {
        st.lt = triggerValue(snap, remap.leftTrigger);
        st.rt = triggerValue(snap, remap.rightTrigger);
    }

    // ── Hat → dpad ──────────────────────────────────────────────────────────
    // SDL_HAT_* is a bitmask, so a diagonal sets two dpad bits.
    const std::uint8_t h =
        (remap.hatIndex >= 0 && snap.hats != nullptr && remap.hatIndex < snap.hatCount)
            ? snap.hats[remap.hatIndex]
            : hat::kCentered;
    if ((h & hat::kUp) != 0) { btn |= B::kDpadUp; }
    if ((h & hat::kDown) != 0) { btn |= B::kDpadDown; }
    if ((h & hat::kLeft) != 0) { btn |= B::kDpadLeft; }
    if ((h & hat::kRight) != 0) { btn |= B::kDpadRight; }

    st.wButtons = btn;
    return st;
}

GamepadInputProcessor::DeviceState mapJoystick(const JoystickSnapshot& snap) {
    return mapJoystick(snap, JoystickRemap{});
}

} // namespace dish::input
