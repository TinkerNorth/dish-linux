// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The per-stream rate estimator behind the live-stats display. Quantized to 5 Hz
// steps so a jittery counter does not flicker the readout every frame. There is
// no clock inside: the caller passes the sample instant in microseconds on any
// monotonic basis, per the hot-path rule that timing never comes from a hidden
// now().

#pragma once

#include <cmath>
#include <cstdint>

namespace dish::reducer {

// std::lround rounds half away from zero, so 22.5 quantizes to 25 deterministically.
inline int quantizeHz(double hz, int step = 5) {
    if (hz <= 0.0) { return 0; }
    if (step <= 1) { return static_cast<int>(std::lround(hz)); }
    const long stepsAway = std::lround(hz / static_cast<double>(step));
    return static_cast<int>(stepsAway) * step;
}

class InputRateTracker {
  public:
    // `count` is the absolute counter value at `nowUs`.
    int sample(std::uint64_t count, std::uint64_t nowUs) {
        if (!hasBaseline_) {
            hasBaseline_ = true;
            lastCount_ = count;
            lastUs_ = nowUs;
            lastHz_ = 0;
            return 0;
        }
        if (count < lastCount_) {
            // The native counter reset or the device re-attached. Rebaseline
            // rather than reporting the spurious huge rate a wrap would produce.
            lastCount_ = count;
            lastUs_ = nowUs;
            lastHz_ = 0;
            return 0;
        }
        if (nowUs <= lastUs_) {
            // No time elapsed, or the clock went backwards. Keep the prior
            // reading but advance the count anchor so the next real interval is
            // measured from here.
            lastCount_ = count;
            return lastHz_;
        }
        const std::uint64_t deltaCount = count - lastCount_;
        const std::uint64_t deltaUs = nowUs - lastUs_;
        const double hz =
            static_cast<double>(deltaCount) * 1'000'000.0 / static_cast<double>(deltaUs);
        lastCount_ = count;
        lastUs_ = nowUs;
        lastHz_ = quantizeHz(hz);
        return lastHz_;
    }

    int lastHz() const { return lastHz_; }

    // Call when a device detaches, since a new one may reuse the slot id.
    void reset() {
        hasBaseline_ = false;
        lastCount_ = 0;
        lastUs_ = 0;
        lastHz_ = 0;
    }

  private:
    bool hasBaseline_ = false;
    std::uint64_t lastCount_ = 0;
    std::uint64_t lastUs_ = 0;
    int lastHz_ = 0;
};

} // namespace dish::reducer
