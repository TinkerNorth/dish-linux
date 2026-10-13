// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/SatelliteClient.h"

#include <cstdint>
#include <optional>

namespace dish {

struct LightbarColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const LightbarColor& o) const { return r == o.r && g == o.g && b == o.b; }
};

// The light bar is independent of rumble: colour arrives on its own stream and is
// gated by its own setting, whichever host sent it. nullopt means apply nothing.
inline std::optional<LightbarColor> lightbarColorFromRgb(std::uint8_t r, std::uint8_t g,
                                                         std::uint8_t b, bool lightbarFollowGame) {
    if (!lightbarFollowGame) { return std::nullopt; }
    return LightbarColor{r, g, b};
}

// A satellite's MSG_LIGHTBAR takes the same gate as a Moonlight host's RGB_LED.
inline std::optional<LightbarColor>
lightbarColorFromLightbarMessage(const net::SatelliteClient::LightbarMessage& lm,
                                 bool lightbarFollowGame) {
    return lightbarColorFromRgb(lm.r, lm.g, lm.b, lightbarFollowGame);
}

} // namespace dish
