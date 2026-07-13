// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure exponential reconnect-backoff schedule (contract/android parity:
// 1s, 2s, 4s, … capped at 60s). Free function so the schedule is unit-testable
// without a clock or timer. Ported from dish-windows core/reducer/Backoff.h;
// AppModel's auto-reconnect tick gates each connection on the deadline this
// schedule produces instead of a blind fixed-period retry.

#pragma once

#include <cstdint>

namespace dish::reducer {

inline constexpr std::int64_t kBackoffBaseMs = 1000;  // first silent retry after 1s
inline constexpr std::int64_t kBackoffMaxMs = 60'000; // capped at 60s
inline constexpr int kBackoffMaxShift = 6;            // 1000<<6 == 64000 → capped to 60000

// Delay before the `attempt`-th consecutive silent retry. `attempt` is 1-based
// (the first retry after a death is attempt 1 → kBackoffBaseMs). Clamped so a
// long outage doesn't overflow the shift or exceed the 60s ceiling. A
// non-positive attempt is treated as the first.
inline std::int64_t backoffDelayMs(int attempt) {
    const int shift =
        (attempt <= 1) ? 0 : ((attempt - 1) > kBackoffMaxShift ? kBackoffMaxShift : (attempt - 1));
    const std::int64_t delay = kBackoffBaseMs << shift;
    return delay > kBackoffMaxMs ? kBackoffMaxMs : delay;
}

} // namespace dish::reducer
