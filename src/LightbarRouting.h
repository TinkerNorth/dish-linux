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

// The light bar is independent of rumble: colour arrives on its own MSG_LIGHTBAR
// stream and is gated by its own setting. nullopt means apply nothing.
inline std::optional<LightbarColor>
lightbarColorFromLightbarMessage(const net::SatelliteClient::LightbarMessage& lm,
                                 bool lightbarFollowGame) {
    if (!lightbarFollowGame) { return std::nullopt; }
    return LightbarColor{lm.r, lm.g, lm.b};
}

} // namespace dish
