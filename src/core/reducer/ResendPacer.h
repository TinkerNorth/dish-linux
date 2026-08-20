// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pacing gate for the overlay/UDP resend loops. Real input is event-driven
// and never passes through here; resends exist only to heal a lost EDGE, the
// final frame of a gesture (button-up, finger-up, stick-to-neutral) that no later
// frame would correct. Not thread-safe: call from the single resend thread. The
// clock is injected (nanoseconds) so the schedule tests against a fake clock.

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace dish::reducer {

class ResendPacer {
  public:
    // One lost edge frame heals at the next tick, a double loss at the one after.
    static constexpr int kEdgeBurstResends = 3;
    // After the burst, a slow keepalive against pathological multi-loss.
    static constexpr std::int64_t kKeepaliveIntervalNs = 1'000'000'000;

    explicit ResendPacer(std::function<std::int64_t()> nanoTime) : nanoTime_(std::move(nanoTime)) {}

    // `changed` is whether the assembled state differs from the last one sent.
    // Advances the internal last-sent clock on every true.
    bool resendDue(bool changed) {
        const std::int64_t now = nanoTime_();
        if (changed) {
            burstLeft_ = kEdgeBurstResends - 1;
        } else if (burstLeft_ > 0) {
            --burstLeft_;
        } else if (now - lastSendNs_ < kKeepaliveIntervalNs) {
            return false;
        }
        lastSendNs_ = now;
        return true;
    }

  private:
    std::function<std::int64_t()> nanoTime_;
    int burstLeft_ = 0;
    std::int64_t lastSendNs_ = 0;
};

} // namespace dish::reducer
