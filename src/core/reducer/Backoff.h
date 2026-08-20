// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The reconnect-backoff schedule: 1s, 2s, 4s ... capped at 60s. Shared with the
// other Dish clients; keep the curve in step with them.

#pragma once

#include <cstdint>

namespace dish::reducer {

inline constexpr std::int64_t kBackoffBaseMs = 1000;  // first silent retry after 1s
inline constexpr std::int64_t kBackoffMaxMs = 60'000; // capped at 60s
inline constexpr int kBackoffMaxShift = 6;            // 1000<<6 == 64000 → capped to 60000

// `attempt` is 1-based. Clamped so a long outage cannot overflow the shift; a
// non-positive attempt is treated as the first.
inline std::int64_t backoffDelayMs(int attempt) {
    const int shift =
        (attempt <= 1) ? 0 : ((attempt - 1) > kBackoffMaxShift ? kBackoffMaxShift : (attempt - 1));
    const std::int64_t delay = kBackoffBaseMs << shift;
    return delay > kBackoffMaxMs ? kBackoffMaxMs : delay;
}

} // namespace dish::reducer
