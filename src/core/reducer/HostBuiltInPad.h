// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A pad that is part of the machine Dish runs on. The Steam Deck's own controls reach SDL through
// its HIDAPI driver as 28de:1205 and sit in the device list beside whatever the user plugged in;
// the setup wizard must not adopt them in place of that pad, and the slot list names them for
// what they are.

#pragma once

#include <cstddef>
#include <vector>

namespace dish::reducer {

inline bool isHostBuiltInPad(int vendorId, int productId) {
    constexpr int kVidValve = 0x28DE;
    constexpr int kPidSteamDeckControls = 0x1205;
    return vendorId == kVidValve && productId == kPidSteamDeckControls;
}

// The pad the setup wizard adopts when nothing seeded it: the first that is not the host's own,
// else the first at all, else -1.
inline int defaultPadIndex(const std::vector<bool>& hostBuiltIn) {
    for (std::size_t i = 0; i < hostBuiltIn.size(); i++) {
        if (!hostBuiltIn[i]) { return static_cast<int>(i); }
    }
    return hostBuiltIn.empty() ? -1 : 0;
}

} // namespace dish::reducer
