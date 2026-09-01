// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What happens to a host's RUMBLE_TRIGGERS packet on a client that cannot reach
// a trigger motor.
//
// The only pad family with impulse-trigger motors is the Xbox One / Series GIP
// pad, and neither desktop platform's Direct path can claim one: on Linux xpad
// binds it as evdev-only and publishes no hidraw node, on Windows XInput hides
// it from raw HID. Every family this client CAN claim (DualShock 4, DualSense,
// Switch Pro) has two body motors and nothing in the triggers. dish-android is
// in a different position -- it reaches GIP pads over USB -- so it actuates the
// real thing there and folds only for its on-screen pad.
//
// Dropping the packet would be the other defensible choice, and it is what this
// client did before: the trigger effect a game asked for is not the effect the
// user feels. The fold wins because a racing game's trigger feedback is often
// the ONLY rumble it sends, and silence reads as broken hardware, while a body
// buzz reads as the game talking to the pad. That is a deliberate trade, so it
// is written down here rather than left implicit in a handler.
//
// The mixing rule is the part that has to be exact: trigger rumble and body
// rumble are two independent streams from the host, each refreshed on its own
// schedule, both landing on the same two motors. Last-writer-wins would let a
// trigger update cancel a body rumble that is still running (and vice versa),
// which is the bug that makes rumble "flicker" under a host that sends both.

#pragma once

#include <cstdint>

namespace dish::moonlight {

// What the two body motors should be driven at.
struct BodyRumble {
    std::uint16_t strong = 0;
    std::uint16_t weak = 0;

    bool operator==(const BodyRumble& o) const { return strong == o.strong && weak == o.weak; }
};

// The live state of both host streams for one controller. Held per (session,
// controller number) by the caller and reset when the pad unbinds.
struct RumbleMix {
    std::uint16_t bodyStrong = 0;
    std::uint16_t bodyWeak = 0;
    // Left trigger folds onto the strong (low-frequency) motor and right onto
    // the weak one, matching how the pads themselves are laid out and what
    // dish-android's virtual pad does, so the three clients feel the same.
    std::uint16_t triggerLeft = 0;
    std::uint16_t triggerRight = 0;

    bool operator==(const RumbleMix& o) const {
        return bodyStrong == o.bodyStrong && bodyWeak == o.bodyWeak &&
               triggerLeft == o.triggerLeft && triggerRight == o.triggerRight;
    }
};

// Per-motor maximum, not a sum: the streams are independent, so neither may
// cancel the other, and adding them would saturate on any game that drives both
// at once. The louder of the two is what the user would have felt anyway.
inline BodyRumble mixRumble(const RumbleMix& mix) {
    BodyRumble out;
    out.strong = mix.bodyStrong > mix.triggerLeft ? mix.bodyStrong : mix.triggerLeft;
    out.weak = mix.bodyWeak > mix.triggerRight ? mix.bodyWeak : mix.triggerRight;
    return out;
}

inline RumbleMix withBodyRumble(RumbleMix mix, std::uint16_t strong, std::uint16_t weak) {
    mix.bodyStrong = strong;
    mix.bodyWeak = weak;
    return mix;
}

inline RumbleMix withTriggerRumble(RumbleMix mix, std::uint16_t left, std::uint16_t right) {
    mix.triggerLeft = left;
    mix.triggerRight = right;
    return mix;
}

} // namespace dish::moonlight
