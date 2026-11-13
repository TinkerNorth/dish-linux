// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A Steam Controller's two trackpads onto the wire, by the slot's pick. The wire
// carries one touch surface per slot (0x000C) beside the XUSB report, so the two
// pads are composed: the 2015 pad's right pad as the right stick with its click
// as R3 (stick, the behaviour before the pick existed), both pads on the one
// surface (pad), the right pad as the cursor and the left as the wheel and the
// right button (mouse), or nowhere (off). A click lands in the extended-buttons
// word wherever nothing else consumes it, so no position and no click ever
// reaches two wire fields; test_steam_trackpads pins that over every pick and
// model. Pure: the decoders feed it raw samples and apply the route.

#pragma once

#include "core/input/ExtendedButtons.h"

#include <algorithm>
#include <cstdint>

namespace dish::input::usbparse {

// The slot's trackpad pick, in the order the store's wire-like strings count them
// (core/reducer/TrackpadPickResolve.h).
enum class TrackpadPick : std::uint8_t { Stick = 0, Pad = 1, Mouse = 2, Off = 3 };

// Which Steam Controller's trackpads a model has: the 2015 pad (its right pad is
// the right stick until picked otherwise), the 2026 pad (a real right stick and
// two pads), or none.
enum class SteamTrackpads : std::uint8_t { None = 0, Pad2015 = 1, Pad2026 = 2 };

// A trackpad as its decoder reads it: up-positive, the full int16 range, SDL's
// shell rotation already applied on the 2015 pads.
struct PadSample {
    bool active = false;
    std::int16_t x = 0;
    std::int16_t y = 0;
    bool click = false;
};

struct SteamPads {
    PadSample left;
    PadSample right;
};

// Mouse mode's wheel: the left pad's vertical travel, one notch per
// kPadScrollUnitsPerNotch, the rest carried to the next sample.
struct PadScrollTracker {
    bool tracking = false;
    std::int16_t lastY = 0;
    std::int32_t remainder = 0;
};

inline constexpr std::int32_t kPadScrollUnitsPerNotch = 4096;
// The wire's notch (proto::kScrollUnitsPerNotch), restated so this header stays
// free of the wire protocol; test_steam_trackpads pins the two equal.
inline constexpr std::int16_t kPadScrollWheelNotch = 120;
inline constexpr std::int16_t kPadHalfSurface = 16384;
inline constexpr std::uint8_t kLeftPadFingerId = 0;
inline constexpr std::uint8_t kRightPadFingerId = 1;

// Where the two pads landed: the 2015 right stick, one touch surface with its
// buttons and wheel, and the pad clicks nothing else consumed.
struct SteamTrackpadRoute {
    std::int16_t rightStickX = 0;
    std::int16_t rightStickY = 0;
    bool rightStickClick = false;
    bool finger0Active = false;
    std::uint8_t finger0Id = kLeftPadFingerId;
    std::int16_t finger0X = 0;
    std::int16_t finger0Y = 0;
    bool finger1Active = false;
    std::uint8_t finger1Id = kRightPadFingerId;
    std::int16_t finger1X = 0;
    std::int16_t finger1Y = 0;
    bool click = false;
    bool rightClick = false;
    std::int16_t scrollV = 0;
    std::uint32_t extButtons = 0;
};

namespace detail {

// Pads are up-positive and the wire is +y down; -(-32768) has no int16.
inline std::int16_t padYToWire(std::int16_t y) {
    return static_cast<std::int16_t>(
        std::clamp<std::int32_t>(-static_cast<std::int32_t>(y), -32767, 32767));
}

inline void clicksToTheWord(const SteamPads& pads, SteamTrackpadRoute& route) {
    if (pads.left.click) { route.extButtons |= extbtn::kLeftPadClick; }
    if (pads.right.click) { route.extButtons |= extbtn::kRightPadClick; }
}

inline void routeAsStick(const SteamPads& pads, SteamTrackpadRoute& route) {
    route.rightStickX = pads.right.active ? pads.right.x : 0;
    route.rightStickY = pads.right.active ? pads.right.y : 0;
    route.rightStickClick = pads.right.click;
    if (pads.left.click) { route.extButtons |= extbtn::kLeftPadClick; }
}

inline void routeAsPad(const SteamPads& pads, SteamTrackpadRoute& route) {
    route.finger0Active = pads.left.active;
    if (pads.left.active) {
        route.finger0X = static_cast<std::int16_t>(pads.left.x / 2 - kPadHalfSurface);
        route.finger0Y = padYToWire(pads.left.y);
    }
    route.finger1Active = pads.right.active;
    if (pads.right.active) {
        route.finger1X = static_cast<std::int16_t>(pads.right.x / 2 + kPadHalfSurface);
        route.finger1Y = padYToWire(pads.right.y);
    }
    route.click = pads.left.click || pads.right.click;
}

inline std::int16_t wheelFrom(const PadSample& left, PadScrollTracker& scroll) {
    if (!left.active) {
        scroll = PadScrollTracker{};
        return 0;
    }
    if (!scroll.tracking) {
        scroll.tracking = true;
        scroll.lastY = left.y;
        return 0;
    }
    scroll.remainder += static_cast<std::int32_t>(left.y) - scroll.lastY;
    scroll.lastY = left.y;
    const std::int32_t notches = scroll.remainder / kPadScrollUnitsPerNotch;
    scroll.remainder -= notches * kPadScrollUnitsPerNotch;
    return static_cast<std::int16_t>(notches * kPadScrollWheelNotch);
}

inline void routeAsMouse(const SteamPads& pads, PadScrollTracker& scroll,
                         SteamTrackpadRoute& route) {
    route.finger0Active = pads.right.active;
    if (pads.right.active) {
        route.finger0X = pads.right.x;
        route.finger0Y = padYToWire(pads.right.y);
    }
    route.click = pads.right.click;
    route.rightClick = pads.left.click;
    route.scrollV = wheelFrom(pads.left, scroll);
}

} // namespace detail

// The 2015 pad's right pad is the right stick only under the stick pick; the
// 2026 pad has a real right stick, so its stick pick reads as off.
inline SteamTrackpadRoute routeSteamTrackpads(TrackpadPick pick, SteamTrackpads model,
                                              const SteamPads& pads, PadScrollTracker& scroll) {
    SteamTrackpadRoute route;
    const bool rightPadIsTheStick = model == SteamTrackpads::Pad2015;
    switch (pick) {
    case TrackpadPick::Stick:
        if (rightPadIsTheStick) {
            detail::routeAsStick(pads, route);
        } else {
            detail::clicksToTheWord(pads, route);
        }
        return route;
    case TrackpadPick::Pad:
        detail::routeAsPad(pads, route);
        return route;
    case TrackpadPick::Mouse:
        detail::routeAsMouse(pads, scroll, route);
        return route;
    case TrackpadPick::Off:
        detail::clicksToTheWord(pads, route);
        return route;
    }
    return route;
}

} // namespace dish::input::usbparse
