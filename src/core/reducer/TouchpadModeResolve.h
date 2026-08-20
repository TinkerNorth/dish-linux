// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The ds4 > mouse > off ladder that turns the user's per-satellite pick into the
// effective `touchpadMode` a slot declares. This client is physical-pads-only, so
// one fact (does the pad have a touchpad SDL can read) gates both rungs.
// TouchpadRouting.h owns the forward payload; this owns only the mode decision.

#pragma once

#include "core/model/Protocol.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace dish::reducer {

// Picker UIs iterate this, so the order is pinned to keep the displayed list
// stable across builds.
inline const std::array<std::string_view, 3> kTouchpadModeNames{
    proto::touchpadModeName(proto::kTouchpadModeOff),
    proto::touchpadModeName(proto::kTouchpadModeDs4),
    proto::touchpadModeName(proto::kTouchpadModeMouse),
};

// Case-sensitive: the satellite compares verbatim. Only a genuine wire string
// survives the round trip unchanged, since unknown names collapse to off.
inline bool isValidTouchpadModeName(std::string_view name) {
    return proto::touchpadModeName(proto::touchpadModeFromName(name)) == name;
}

// A touchpad-bearing type with no "ds4" lineage gates the pad routing off. A
// pre-modes catalog omits `modes` entirely; empty therefore means pad-capable.
inline bool typeOffersDs4Touchpad(bool supported, const std::vector<std::string>& modes) {
    if (!supported) { return false; }
    if (modes.empty()) { return true; }
    return std::find(modes.begin(), modes.end(),
                     std::string(proto::touchpadModeName(proto::kTouchpadModeDs4))) != modes.end();
}

// A pick the path cannot carry declares off rather than a request the satellite
// would dead-letter, and a blocked pick NEVER falls back to the other routing.
// The two rungs read disjoint gates on purpose: the pad routing is a per-type
// concern, mouse control is host-injected.
inline std::uint8_t resolveTouchpadMode(std::string_view pick, bool padHasTouchpad,
                                        bool typeOffersDs4, bool hostMouseControl) {
    if (pick == proto::touchpadModeName(proto::kTouchpadModeDs4) && padHasTouchpad &&
        typeOffersDs4) {
        return proto::kTouchpadModeDs4;
    }
    if (pick == proto::touchpadModeName(proto::kTouchpadModeMouse) && padHasTouchpad &&
        hostMouseControl) {
        return proto::kTouchpadModeMouse;
    }
    return proto::kTouchpadModeOff;
}

} // namespace dish::reducer
