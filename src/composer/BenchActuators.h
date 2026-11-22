// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the Diagnostics bench drives a pad through, and the clock it runs on:
// the seams FeedbackBenchCoordinator takes, so a test can stand in for the
// router and for time.

#pragma once

#include "core/audio/PadAudioMatcher.h"
#include "core/reducer/BenchSurfaces.h"
#include "core/reducer/FeedbackBench.h"

#include <QString>

#include <cstdint>
#include <functional>

namespace dish::composer {

// The router's bench half: the motors, the state surfaces a test holds and gives back, and the
// pad's audio route. GUI thread.
class BenchActuators {
  public:
    virtual ~BenchActuators() = default;
    virtual void buzz(const QString& slotId, const reducer::TestBuzz& buzz) = 0;
    virtual void triggerBuzz(const QString& slotId, const reducer::TestTriggerBuzz& buzz) = 0;
    virtual void holdSurface(const QString& slotId, reducer::BenchSurface surface) = 0;
    virtual void writeSurface(const QString& slotId, const reducer::SurfaceWrite& write) = 0;
    virtual void giveBackSurface(const QString& slotId, reducer::BenchSurface surface) = 0;
    virtual audio::PadAudioRoute audioRouteFor(const QString& slotId) const = 0;

  protected:
    BenchActuators() = default;
    BenchActuators(const BenchActuators&) = default;
    BenchActuators& operator=(const BenchActuators&) = default;
    BenchActuators(BenchActuators&&) = default;
    BenchActuators& operator=(BenchActuators&&) = default;
};

// When a test's steps fall due, and the wall-clock time its outcome is stamped with.
class BenchClock {
  public:
    virtual ~BenchClock() = default;
    virtual std::int64_t nowMs() const = 0;
    virtual void after(std::int64_t delayMs, std::function<void()> due) = 0;

  protected:
    BenchClock() = default;
    BenchClock(const BenchClock&) = default;
    BenchClock& operator=(const BenchClock&) = default;
    BenchClock(BenchClock&&) = default;
    BenchClock& operator=(BenchClock&&) = default;
};

} // namespace dish::composer
