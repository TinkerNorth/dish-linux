// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The MSG_TOUCHPAD (0x000C) forward payload for a real controller's two-finger
// pad. Touchpad input is event-driven and neither rate-limited nor coalesced, so
// there is no forwarding gate beyond "a sample arrived"; what matters here is
// that eventTimeMs is carried end to end.

#pragma once

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

    bool operator==(const TouchpadForward& o) const {
        return finger0Active == o.finger0Active && finger0Id == o.finger0Id &&
               finger0X == o.finger0X && finger0Y == o.finger0Y &&
               finger1Active == o.finger1Active && finger1Id == o.finger1Id &&
               finger1X == o.finger1X && finger1Y == o.finger1Y &&
               buttonPressed == o.buttonPressed && eventTimeMs == o.eventTimeMs;
    }
};

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
