// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/SatelliteClient.h"

#include <cstdint>

namespace dish::net {

// Definition of the test-only friend seam declared in SatelliteClient.h.
// Shared by every test TU that needs it (one definition — ODR).
class SatelliteClientTestAccess {
  public:
    // Park the 64-bit send counter so exhaustion is reachable without four
    // billion sends. `next` is the next value sendEncrypted will draw.
    static void seedSendCounter(SatelliteClient& client, std::uint64_t next) {
        client.sendCounter_.reset(next);
    }
};

} // namespace dish::net
