// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Is anyone actually playing? The input threads bump a monotonic actuation
// counter; this samples it on the GUI thread against the configured idle
// window. No clock inside — sampleAt() takes `now`, so tests pump it.

#pragma once

#include "architecture/StateSource.h"
#include "core/reducer/KeepAwake.h"

#include <cstdint>
#include <functional>
#include <utility>

namespace dish::source {

class ControllerActivitySource : public arch::StateSource<bool> {
  public:
    using ActuationCounter = std::function<std::uint64_t()>;

    explicit ControllerActivitySource(ActuationCounter counter)
        : arch::StateSource<bool>(false), counter_(std::move(counter)),
          lastCount_(counter_ ? counter_() : 0) {}

    void setIdleTimeoutMinutes(int minutes) {
        timeoutMs_ = static_cast<std::int64_t>(reducer::clampKeepAwakeTimeoutMinutes(minutes)) *
                     kMsPerMinute;
    }

    // A session opening is activity: the user just did something, and the pad
    // they are about to pick up has not reported yet.
    void noteActivityAt(std::int64_t nowMs) {
        stamp(nowMs);
        publishAt(nowMs);
    }

    void sampleAt(std::int64_t nowMs) {
        if (counter_) {
            if (const std::uint64_t count = counter_(); count != lastCount_) {
                lastCount_ = count;
                stamp(nowMs);
            }
        }
        publishAt(nowMs);
    }

  private:
    static constexpr std::int64_t kMsPerMinute = 60'000;

    void stamp(std::int64_t nowMs) {
        lastInputMs_ = nowMs;
        hasInput_ = true;
    }

    void publishAt(std::int64_t nowMs) {
        setState(hasInput_ && reducer::controllerActiveAt(lastInputMs_, nowMs, timeoutMs_));
    }

    ActuationCounter counter_;
    std::uint64_t lastCount_ = 0;
    // A separate flag, not a lastInputMs_ == 0 sentinel: 0 is a legitimate
    // monotonic stamp, and a second sample would look like another first one.
    bool hasInput_ = false;
    std::int64_t lastInputMs_ = 0;
    std::int64_t timeoutMs_ =
        static_cast<std::int64_t>(reducer::kKeepAwakeDefaultTimeoutMinutes) * kMsPerMinute;
};

} // namespace dish::source
