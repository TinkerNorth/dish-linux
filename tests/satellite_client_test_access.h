// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/SatelliteClient.h"

#include <cstdint>

namespace dish::net {

// The one definition of the test-only friend seam declared in SatelliteClient.h,
// shared by every test TU that needs it (ODR).
class SatelliteClientTestAccess {
  public:
    // Parks the send counter so nonce exhaustion is reachable without 2^64 sends.
    // `next` is the value sendEncrypted draws next.
    static void seedSendCounter(SatelliteClient& client, std::uint64_t next) {
        client.sendCounter_.reset(next);
    }
};

} // namespace dish::net
