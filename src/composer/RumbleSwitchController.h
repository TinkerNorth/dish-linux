// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Stops a slot's motors the moment its rumble switch goes off. Every later host
// command already arrives as a stop (reducer::rumbleTheUserAllows), but a
// Moonlight host sends only on change and a Direct claim's levels never expire,
// so a motor running when the switch went off would otherwise run on until the
// host next spoke.

#pragma once

#include "architecture/Controller.h"
#include "source/store/RumbleEnabledStore.h"

#include <QString>

#include <functional>

namespace dish::composer {

class RumbleSwitchController : public arch::Controller<source::RumbleEnabledMap> {
  public:
    // Runs on the thread that flips the switch.
    using StopMotors = std::function<void(const QString& slotId)>;

    // The switches as they stand at construction are the baseline, so start()
    // stops nothing: a switch that was off before launch has no motor running.
    RumbleSwitchController(const arch::Observable<source::RumbleEnabledMap>& switches,
                           StopMotors stopMotors);

  protected:
    void apply(const source::RumbleEnabledMap& switches) override;

  private:
    StopMotors stopMotors_;
    source::RumbleEnabledMap seen_;
};

} // namespace dish::composer
