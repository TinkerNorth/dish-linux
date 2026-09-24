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

namespace {

JoystickRemap withButton(JoystickRemap base, RemapButton b, int index) {
    base.buttons[idx(b)] = index;
    return base;
}

// A Hat capture points the dpad at that hat and drops the direction's button override so the hat
// wins; a Button capture leaves hatIndex alone, so the other directions keep reading the hat.
JoystickRemap withDpad(JoystickRemap base, RemapButton b, int kind, int index) {
    if (kind != static_cast<int>(CaptureKind::Hat)) { return withButton(base, b, index); }
    base.hatIndex = index;
    base.buttons[idx(b)] = -1;
    return base;
}

// Either way the explicit choice disables the adaptive fallback.
JoystickRemap withTrigger(JoystickRemap base, TriggerSource JoystickRemap::* which, int kind,
                          int index) {
    TriggerSource& t = base.*which;
    t.kind = (kind == static_cast<int>(CaptureKind::Button)) ? TriggerSourceKind::Button
                                                             : TriggerSourceKind::Axis;
    t.index = index;
    base.useAdaptiveTriggers = false;
    return base;
}

// An explicit right stick disables the adaptive guess the same way a trigger does.
JoystickRemap withRightStick(JoystickRemap base, int JoystickRemap::* axis, int index) {
    base.*axis = index;
    base.useAdaptiveRightStick = false;
    return base;
}

} // namespace

JoystickRemap withAssignment(JoystickRemap base, RemapTarget target, int kind, int index) {
    switch (target) {
    case RemapTarget::A:
        return withButton(base, RemapButton::A, index);
    case RemapTarget::B:
        return withButton(base, RemapButton::B, index);
    case RemapTarget::X:
        return withButton(base, RemapButton::X, index);
    case RemapTarget::Y:
        return withButton(base, RemapButton::Y, index);
    case RemapTarget::DpadUp:
        return withDpad(base, RemapButton::DpadUp, kind, index);
    case RemapTarget::DpadDown:
        return withDpad(base, RemapButton::DpadDown, kind, index);
    case RemapTarget::DpadLeft:
        return withDpad(base, RemapButton::DpadLeft, kind, index);
    case RemapTarget::DpadRight:
        return withDpad(base, RemapButton::DpadRight, kind, index);
    case RemapTarget::LeftShoulder:
        return withButton(base, RemapButton::LeftShoulder, index);
    case RemapTarget::RightShoulder:
        return withButton(base, RemapButton::RightShoulder, index);
    case RemapTarget::Back:
        return withButton(base, RemapButton::Back, index);
    case RemapTarget::Start:
        return withButton(base, RemapButton::Start, index);
    case RemapTarget::LeftThumb:
        return withButton(base, RemapButton::LeftThumb, index);
    case RemapTarget::RightThumb:
        return withButton(base, RemapButton::RightThumb, index);
    case RemapTarget::LeftStickX:
        base.leftStickX = index;
        return base;
    case RemapTarget::LeftStickY:
        base.leftStickY = index;
        return base;
    case RemapTarget::RightStickX:
        return withRightStick(base, &JoystickRemap::rightStickX, index);
    case RemapTarget::RightStickY:
        return withRightStick(base, &JoystickRemap::rightStickY, index);
    case RemapTarget::LeftTrigger:
        return withTrigger(base, &JoystickRemap::leftTrigger, kind, index);
    case RemapTarget::RightTrigger:
        return withTrigger(base, &JoystickRemap::rightTrigger, kind, index);
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
