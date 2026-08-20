// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Turns a running per-device transfer-completion counter into a measured poll
// rate. The IO half (the timer that reads the counters) lives in the driver at
// source/usb/UsbGamepadManager; this is the clock-injected decision half.

#pragma once

#include "core/reducer/UsbPollRate.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dish::reducer {

class PollRateSampler {
  public:
    struct RateUpdate {
        int deviceId = 0;
        int rateHz = 0;
    };

    // `countOf` reads a device's current completion counter. A device's first
    // sample only seeds the baseline and emits nothing.
    std::vector<RateUpdate> sampleAll(std::int64_t nowMs, const std::vector<int>& present,
                                      const std::function<std::int64_t(int)>& countOf) {
        std::vector<RateUpdate> updates;
        for (int id : present) {
            const std::int64_t count = countOf(id);
            const auto it = snapshots_.find(id);
            const bool hadPrev = it != snapshots_.end();
            Snapshot prev{};
            if (hadPrev) { prev = it->second; }
            snapshots_[id] = Snapshot{count, nowMs};
            if (!hadPrev) { continue; }
            updates.push_back(
                RateUpdate{id, measuredPollRateHz(count - prev.count, nowMs - prev.elapsedMs)});
        }
        // Retire absent devices, so a later re-attach of the same id starts from
        // a fresh snapshot rather than a stale baseline.
        retainOnly(present);
        return updates;
    }

    // For an explicit detach between sample ticks.
    void forget(int deviceId) { snapshots_.erase(deviceId); }

    void reset() { snapshots_.clear(); }

  private:
    struct Snapshot {
        std::int64_t count = 0;
        std::int64_t elapsedMs = 0;
    };

    void retainOnly(const std::vector<int>& present) {
        for (auto it = snapshots_.begin(); it != snapshots_.end();) {
            bool keep = false;
            for (int id : present) {
                if (id == it->first) {
                    keep = true;
                    break;
                }
            }
            if (keep) {
                ++it;
            } else {
                it = snapshots_.erase(it);
            }
        }
    }

    std::unordered_map<int, Snapshot> snapshots_;
};

} // namespace dish::reducer
