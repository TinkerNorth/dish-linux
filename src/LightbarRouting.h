// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/SatelliteClient.h"

#include <cstdint>
#include <optional>

namespace dish {

// A resolved lightbar colour destined for the SDL bridge's applyLightbar.
struct LightbarColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const LightbarColor& o) const { return r == o.r && g == o.g && b == o.b; }
};

// Task 1.4 lightbar routing — the pure decision layer behind AppModel's
// lightbar handler and exercised directly by unit tests.
//
// The light bar is independent of rumble: colour arrives on the dedicated
// MSG_LIGHTBAR stream and is gated by its own on/off setting. The function
// below encodes exactly that: it returns the colour to push to applyLightbar,
// or std::nullopt when nothing should be applied.

// Colour to apply for a decoded MSG_LIGHTBAR message. `lightbarFollowGame` is
// the FeatureSettings gate (true = "Follow game", false = "Off"). Returns
// nullopt when the light bar is Off.
inline std::optional<LightbarColor>
lightbarColorFromLightbarMessage(const net::SatelliteClient::LightbarMessage& lm,
                                 bool lightbarFollowGame) {
    if (!lightbarFollowGame) { return std::nullopt; }
    return LightbarColor{lm.r, lm.g, lm.b};
}

} // namespace dish
