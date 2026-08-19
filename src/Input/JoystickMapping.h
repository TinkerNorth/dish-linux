// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "GamepadInputProcessor.h"

#include <array>
#include <cstdint>

namespace dish::input {

// Pure, SDL-free mapper for RAW SDL *joysticks* — pads SDL classifies as a
// joystick but NOT a game controller (no entry in SDL's controller-mapping DB).
// SDLGamepadBridge::rebuildState covers recognised pads; this fills the gap so
// a generic pad still streams through the same GamepadInputProcessor::publish.
//
// Total and deterministic: any index the layout references but the device does
// not provide reads neutral, so a pad with fewer axes or buttons than the
// default assumes can never read out of range.
//
// The JoystickRemap defaults below ARE the default layout — the common
// DirectInput "Generic USB Joystick" order. It is a best guess, because real
// generic pads vary wildly; the per-device remap exists to correct it.

// The bridge fills this from SDL so the mapper never touches SDL. A null
// pointer with a non-zero count reads all-neutral, keeping the mapper total
// even on a malformed snapshot.
struct JoystickSnapshot {
    const std::int16_t* axes = nullptr; // length = axisCount
    int axisCount = 0;
    const bool* buttons = nullptr; // length = buttonCount
    int buttonCount = 0;
    const std::uint8_t* hats = nullptr; // length = hatCount (SDL_HAT_* bitmask)
    int hatCount = 0;
};

// Mirrored from SDL_hat.h so this header stays SDL-free; they are stable
// public constants in SDL2, not a detail that can drift.
namespace hat {
constexpr std::uint8_t kCentered = 0x00;
constexpr std::uint8_t kUp = 0x01;
constexpr std::uint8_t kRight = 0x02;
constexpr std::uint8_t kDown = 0x04;
constexpr std::uint8_t kLeft = 0x08;
} // namespace hat

// ── Per-device REMAP ────────────────────────────────────────────────────────
// The user-correctable routing table the "Configure controls" page edits: which
// RAW source (axis / button / hat index) drives each logical output, so a user
// can fix a pad whose generic DirectInput order is scrambled.

// A generic pad exposes a trigger as either an analogue axis or a digital
// button; tagging which picks triggerFromAxis vs full-scale-on-press.
enum class TriggerSourceKind { Axis, Button };

struct TriggerSource {
    TriggerSourceKind kind = TriggerSourceKind::Axis;
    int index = -1; // -1 = unassigned → reads neutral (trigger stays 0)

    bool operator==(const TriggerSource& o) const { return kind == o.kind && index == o.index; }
    bool operator!=(const TriggerSource& o) const { return !(*this == o); }
};

enum class RemapButton : int {
    DpadUp = 0,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    LeftThumb,
    RightThumb,
    LeftShoulder,
    RightShoulder,
    A,
    B,
    X,
    Y,
    kButtonCount // sentinel: array size, not a real button
};
constexpr int kRemapButtonCount = static_cast<int>(RemapButton::kButtonCount);

// Indices are RAW source indices into the snapshot; -1 means unassigned and
// the logical output reads neutral.
struct JoystickRemap {
    // Right-stick defaults are the 6-axis (dedicated-trigger-axes) layout;
    // useAdaptiveRightStick swaps them to 2/3 on a pad reporting < 6 axes.
    int leftStickX = 0;
    int leftStickY = 1;
    int rightStickX = 3;
    int rightStickY = 4;
    bool invertLeftY = true; // SDL Y is +down; the XUSB report expects +up
    bool invertRightY = true;

    // Axes 2/5 is the >= 6-axis layout; useAdaptiveTriggers falls back to
    // buttons 8/9 at full scale below that.
    TriggerSource leftTrigger{TriggerSourceKind::Axis, 2};
    TriggerSource rightTrigger{TriggerSourceKind::Axis, 5};

    // The DirectInput face-first order. Dpad entries default to -1 because the
    // dpad comes from the HAT; a remap may route a direction to a button.
    std::array<int, kRemapButtonCount> buttons{/*DpadUp*/ -1,
                                               /*DpadDown*/ -1,
                                               /*DpadLeft*/ -1,
                                               /*DpadRight*/ -1,
                                               /*Start*/ 7,
                                               /*Back*/ 6,
                                               /*LeftThumb*/ 10,
                                               /*RightThumb*/ 11,
                                               /*LeftShoulder*/ 4,
                                               /*RightShoulder*/ 5,
                                               /*A*/ 0,
                                               /*B*/ 1,
                                               /*X*/ 2,
                                               /*Y*/ 3};

    int hatIndex = 0; // raw hat index that drives the dpad (-1 = no hat dpad)

    // Not user-editable: true only while the group is at its default, cleared
    // the moment the user assigns it. They let one default value serve both the
    // 6-axis and the 4/5-axis pads while an explicit remap is applied verbatim.
    bool useAdaptiveRightStick = true; // right stick falls back to 2/3 on < 6 axes
    bool useAdaptiveTriggers = true;   // triggers fall back to buttons 8/9 on < 6 axes

    bool operator==(const JoystickRemap& o) const {
        return leftStickX == o.leftStickX && leftStickY == o.leftStickY &&
               rightStickX == o.rightStickX && rightStickY == o.rightStickY &&
               invertLeftY == o.invertLeftY && invertRightY == o.invertRightY &&
               leftTrigger == o.leftTrigger && rightTrigger == o.rightTrigger &&
               buttons == o.buttons && hatIndex == o.hatIndex &&
               useAdaptiveRightStick == o.useAdaptiveRightStick &&
               useAdaptiveTriggers == o.useAdaptiveTriggers;
    }
    bool operator!=(const JoystickRemap& o) const { return !(*this == o); }
};

// ── Capture → assignment (pure, testable) ───────────────────────────────────
// "Configure controls" runs a CAPTURE: the bridge streams the raw input the
// user just pressed and withAssignment folds it into a JoystickRemap. Kept
// SDL-free so the whole capture→remap step is testable without a live device.

// Mirrors the bridge's rawJoystickInput `kind` argument so the UI never
// hard-codes the integers.
enum class CaptureKind : int { Axis = 0, Button = 1, Hat = 2 };

// The invert toggles are absent by design: they are booleans, not a captured
// source, so they go through withInvert instead.
enum class RemapTarget : int {
    A = 0,
    B,
    X,
    Y,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftShoulder,
    RightShoulder,
    Back,
    Start,
    LeftThumb,
    RightThumb,
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    LeftTrigger,
    RightTrigger,
};

enum class InvertTarget : int { LeftY = 0, RightY };

// Folds one capture (kind + raw source index) into `base`. Total, and it
// records the user's choice verbatim: routing a button capture to a stick
// target, or an axis capture to a digital button, is honoured — deciding what
// makes sense is the page's job, not this function's. A trigger target takes
// its TriggerSource kind from the capture kind, so a button-sourced trigger
// decodes at full-scale-on-press. Assigning a stick or trigger clears the
// matching adaptive fallback. A Hat capture on a dpad target points the dpad at
// that hat and drops the direction's button override; a Button capture routes
// the direction to the button.
JoystickRemap withAssignment(JoystickRemap base, RemapTarget target, int kind, int index);

JoystickRemap withInvert(JoystickRemap base, InvertTarget which, bool on);

// Deliberate-press gate so resting-axis jitter never self-assigns. Button and
// hat captures do not need it — see the two predicates below.
bool captureAxisPasses(int value);

// Always true: SDL only raises SDL_JOYBUTTONDOWN on a real press. A named
// predicate so the bridge and the tests share one definition.
bool captureButtonPasses();

// A release back to center is not an assignment. `hatValue` is an SDL_HAT_*
// bitmask.
bool captureHatPasses(int hatValue);

GamepadInputProcessor::DeviceState mapJoystick(const JoystickSnapshot& snap,
                                               const JoystickRemap& remap);

// Maps under the default remap.
GamepadInputProcessor::DeviceState mapJoystick(const JoystickSnapshot& snap);

// ── Internal helpers, exposed for unit tests (pure) ─────────────────────────

// Both read neutral for an out-of-range index or a null array, so the mapper
// never reads past the device's real counts.
std::int16_t axisAt(const JoystickSnapshot& snap, int index);
bool buttonAt(const JoystickSnapshot& snap, int index);

// Maps the positive half of the int16 span to 0..255. Negative reads 0 because
// most generic pads rest a trigger axis at the negative extreme.
std::uint8_t triggerFromAxis(std::int16_t v);

// >= 6 axes, the DirectInput signature for two sticks plus two dedicated
// trigger axes. Drives whether triggers come from axes or from buttons.
bool hasTriggerAxes(const JoystickSnapshot& snap);

} // namespace dish::input
