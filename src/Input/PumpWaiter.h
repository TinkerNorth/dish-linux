// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/input/PumpPacing.h"

namespace dish::input {

// Waits for the pump's next deadline: in clock_nanosleep to the absolute deadline, or by spinning
// on one core when the policy asks for it. Owned and used by the SDL thread alone.
class PumpWaiter {
  public:
    explicit PumpWaiter(pump::WaitStrategy strategy) : strategy_(strategy) {}

    // Returns at or after `deadline`, at once when it has passed.
    void waitUntil(pump::Clock::time_point deadline) const;

  private:
    const pump::WaitStrategy strategy_;
};

} // namespace dish::input
