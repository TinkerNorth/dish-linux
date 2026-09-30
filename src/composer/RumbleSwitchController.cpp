// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/RumbleSwitchController.h"

namespace dish::composer {

namespace {

bool wasOn(const source::RumbleEnabledMap& seen, const QString& slotId) {
    const auto it = seen.find(slotId);
    if (it == seen.end()) { return source::RumbleEnabledStore::kDefaultEnabled; }
    return it->second;
}

} // namespace

RumbleSwitchController::RumbleSwitchController(
    const arch::Observable<source::RumbleEnabledMap>& switches, StopMotors stopMotors)
    : arch::Controller<source::RumbleEnabledMap>(switches), stopMotors_(std::move(stopMotors)),
      seen_(switches.value()) {}

void RumbleSwitchController::apply(const source::RumbleEnabledMap& switches) {
    for (const auto& [slotId, on] : switches) {
        const bool switchedOff = !on && wasOn(seen_, slotId);
        if (switchedOff) { stopMotors_(slotId); }
    }
    seen_ = switches;
}

} // namespace dish::composer
