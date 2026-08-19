// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The input-capture lifecycle ("press a button to assign it") as one value.
// Capture is a one-shot: an accepted press returns to Idle and the coordinator
// folds the (kind, index) through JoystickMapping::withAssignment, then re-arms.
// The accept threshold reuses the bridge's own capture*Passes predicates so the
// deliberate-press gate is defined once.

#pragma once

#include "Input/JoystickMapping.h" // CaptureKind + capture{Axis,Button,Hat}Passes

#include <string>
#include <variant>

namespace dish::input {

enum class CapturePhase { Idle, Capturing };

// For an SDL raw joystick the slot id IS the "sdl:<iid>" device id, which is what
// makes the deviceId == slotId filter below work. Both fields are empty in Idle.
struct CaptureState {
    CapturePhase phase = CapturePhase::Idle;
    std::string slotId;
    std::string target;

    bool operator==(const CaptureState& o) const {
        return phase == o.phase && slotId == o.slotId && target == o.target;
    }
    bool operator!=(const CaptureState& o) const { return !(*this == o); }
};

// ── Events ────────────────────────────────────────────────────────────────

namespace capture_event {

struct Start {
    std::string slotId;
    std::string target;
    bool operator==(const Start& o) const { return slotId == o.slotId && target == o.target; }
    bool operator!=(const Start& o) const { return !(*this == o); }
};

// The rawJoystickInput payload 1:1. `kind` is a CaptureKind; `value` is the axis
// int16, 1 for a button press, or the SDL_HAT_* bitmask for a hat.
struct RawInput {
    std::string deviceId;
    int kind = 0;
    int index = 0;
    int value = 0;
    bool operator==(const RawInput& o) const {
        return deviceId == o.deviceId && kind == o.kind && index == o.index && value == o.value;
    }
    bool operator!=(const RawInput& o) const { return !(*this == o); }
};

struct Stop {
    bool operator==(const Stop&) const { return true; }
    bool operator!=(const Stop&) const { return false; }
};

} // namespace capture_event

using CaptureEvent =
    std::variant<capture_event::Start, capture_event::RawInput, capture_event::Stop>;

// `accepted` is true only for a RawInput that, while Capturing, came from the
// matching device and cleared the deliberate-press threshold. The coordinator
// reads the captured (kind, index) off the event it just fed in.
struct CaptureReduction {
    CaptureState next;
    bool accepted = false;

    bool operator==(const CaptureReduction& o) const {
        return next == o.next && accepted == o.accepted;
    }
    bool operator!=(const CaptureReduction& o) const { return !(*this == o); }
};

// An unrecognised kind fails closed, so the reducer stays total and a bogus kind
// can never self-assign.
inline bool capturePasses(int kind, int value) {
    switch (static_cast<CaptureKind>(kind)) {
    case CaptureKind::Axis:
        return captureAxisPasses(value);
    case CaptureKind::Button:
        return captureButtonPasses();
    case CaptureKind::Hat:
        return captureHatPasses(value);
    }
    return false;
}

// Total over every (phase x event). A Start while already Capturing re-targets
// rather than being rejected; Stop is idempotent, so a double-clear is harmless.
inline CaptureReduction reduceCapture(const CaptureState& state, const CaptureEvent& event) {
    if (const auto* start = std::get_if<capture_event::Start>(&event)) {
        return CaptureReduction{CaptureState{CapturePhase::Capturing, start->slotId, start->target},
                                /*accepted=*/false};
    }
    if (std::get_if<capture_event::Stop>(&event) != nullptr) {
        return CaptureReduction{CaptureState{CapturePhase::Idle, /*slotId=*/{}, /*target=*/{}},
                                /*accepted=*/false};
    }
    if (const auto* raw = std::get_if<capture_event::RawInput>(&event)) {
        // The device filter is what stops a second pad from assigning itself to
        // the slot being configured.
        if (state.phase == CapturePhase::Capturing && raw->deviceId == state.slotId &&
            capturePasses(raw->kind, raw->value)) {
            return CaptureReduction{CaptureState{CapturePhase::Idle, /*slotId=*/{}, /*target=*/{}},
                                    /*accepted=*/true};
        }
        // Idle, a different device, or a sub-threshold press: ignore, unchanged.
        return CaptureReduction{state, /*accepted=*/false};
    }
    return CaptureReduction{state, /*accepted=*/false};
}

} // namespace dish::input
