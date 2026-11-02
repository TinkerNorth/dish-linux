// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/WifiConnection.h"

namespace dish::net {

// The one definition of the test-only friend seam declared in WifiConnection.h, shared by every
// test TU that needs it (ODR).
class WifiConnectionTestAccess {
  public:
    // One alive tick, as the session's timer fires it.
    static void tick(WifiConnection& conn) { conn.onAliveTick(); }
};

} // namespace dish::net
