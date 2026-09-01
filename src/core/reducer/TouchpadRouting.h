// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The 0x000C forward payload for a real controller's two-finger pad, in both
// protocol generations: MSG_TOUCHPAD (v1, 16B) and the reshaped POINTER frame
// (v2, 19B). Touchpad input is event-driven and neither rate-limited nor
// coalesced, so there is no forwarding gate beyond "a sample arrived"; what
// matters here is that eventTimeMs is carried end to end.
//
// v2 moved the click out of the finger flags into a buttons byte and appended a
// signed wheel. This client's ONLY producer is a physical pad's touchpad, which
// reports one click and no wheel, so `rightPressed`, `middlePressed` and
// `scrollV` are structurally absent here rather than synthesized from gestures:
// inventing a two-finger right-click would send the host a button the user
// never pressed, and dish-android forwards a physical pad's pad the same way
// (its extra buttons come from the on-screen trackpad, which this app has no
// equivalent of). The fields exist so the encoder's shape is the wire's shape
// and a future producer has somewhere to put them.

#pragma once

#include "core/model/Protocol.h"

#include <cstdint>

namespace dish::reducer {

// v1 ships with mouse control off: the pad is forwarded as a touchpad, never as a
// mouse. The session descriptor sends hostFeatures.mouseControl from this.
inline constexpr bool kTouchpadMouseControlV1 = false;

// Mirrors SatelliteClient::encodeTouchpadPayload's argument list, so the
// routing-to-encoder seam is a single value.
struct TouchpadForward {
    bool finger0Active = false;
    std::uint8_t finger0Id = 0;
    std::int16_t finger0X = 0;
    std::int16_t finger0Y = 0;
    bool finger1Active = false;
    std::uint8_t finger1Id = 0;
    std::int16_t finger1X = 0;
    std::int16_t finger1Y = 0;
    bool buttonPressed = false;
    // Protocol-1 trailing field, u32 LE at offset 11 of the payload. The server
    // drops the whole packet if it is missing. Stamp it with monotonic uptime ms
    // per publish: mouse-mode timing scales by the delta between samples.
    std::uint32_t eventTimeMs = 0;

    // v2 only, and never set by a physical pad's touchpad (see the file header).
    // A v1 session drops them: the frame has nowhere to put them.
    bool rightPressed = false;
    bool middlePressed = false;
    // An event, not a level: kScrollUnitsPerNotch per detent, 0 on a resend.
    std::int16_t scrollV = 0;

    bool operator==(const TouchpadForward& o) const {
        return finger0Active == o.finger0Active && finger0Id == o.finger0Id &&
               finger0X == o.finger0X && finger0Y == o.finger0Y &&
               finger1Active == o.finger1Active && finger1Id == o.finger1Id &&
               finger1X == o.finger1X && finger1Y == o.finger1Y &&
               buttonPressed == o.buttonPressed && eventTimeMs == o.eventTimeMs &&
               rightPressed == o.rightPressed && middlePressed == o.middlePressed &&
               scrollV == o.scrollV;
    }
};

// The v2 fingerFlags byte. The click bit that lived at 0x04 in v1 is gone.
inline std::uint8_t pointerFingerFlags(const TouchpadForward& f) {
    std::uint8_t flags = 0;
    if (f.finger0Active) { flags |= proto::kPointerFinger0Active; }
    if (f.finger1Active) { flags |= proto::kPointerFinger1Active; }
    return flags;
}

// The v2 buttons byte. `buttonPressed` IS the left button: on a controller
// touchpad the physical click and a left click are the same event.
inline std::uint8_t pointerButtons(const TouchpadForward& f) {
    std::uint8_t buttons = 0;
    if (f.buttonPressed) { buttons |= proto::kPointerButtonLeft; }
    if (f.rightPressed) { buttons |= proto::kPointerButtonRight; }
    if (f.middlePressed) { buttons |= proto::kPointerButtonMiddle; }
    return buttons;
}

// Deliberately does not zero an inactive finger's id or coords: the active flags
// are the sole source of truth, matching the encoder's layout contract.
inline TouchpadForward assembleTouchpadForward(bool finger0Active, std::uint8_t finger0Id,
                                               std::int16_t finger0X, std::int16_t finger0Y,
                                               bool finger1Active, std::uint8_t finger1Id,
                                               std::int16_t finger1X, std::int16_t finger1Y,
                                               bool buttonPressed, std::uint32_t eventTimeMs) {
    TouchpadForward f;
    f.finger0Active = finger0Active;
    f.finger0Id = finger0Id;
    f.finger0X = finger0X;
    f.finger0Y = finger0Y;
    f.finger1Active = finger1Active;
    f.finger1Id = finger1Id;
    f.finger1X = finger1X;
    f.finger1Y = finger1Y;
    f.buttonPressed = buttonPressed;
    f.eventTimeMs = eventTimeMs;
    return f;
}

} // namespace dish::reducer
