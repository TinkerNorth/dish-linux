// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/WakeStateController.h"

#include "source/system/WakeInhibitor.h"

namespace dish::composer {

WakeStateController::WakeStateController(const arch::Observable<WakeState>& wakeState,
                                         source::WakeInhibitor* inhibitor, QString reason)
    : arch::Controller<WakeState>(wakeState), inhibitor_(inhibitor), reason_(std::move(reason)) {}

reducer::KeepAwakeReach WakeStateController::held() const {
    return inhibitor_ == nullptr ? reducer::KeepAwakeReach::None : inhibitor_->held();
}

bool WakeStateController::isInhibiting() const { return held() != reducer::KeepAwakeReach::None; }

void WakeStateController::apply(const WakeState& value) {
    // Belt-and-braces: the kernel already tore the subscription down on stop().
    if (stopped_ || inhibitor_ == nullptr) { return; }
    // apply is idempotent, so the OS is only touched on a real move.
    inhibitor_->apply(value.reach, reason_);
}

void WakeStateController::onStarting() {
    // Re-arm so a restarted controller actuates again.
    stopped_ = false;
}

void WakeStateController::stop() {
    stopped_ = true;
    cancelCollection();
    if (inhibitor_ != nullptr) { inhibitor_->apply(reducer::KeepAwakeReach::None, reason_); }
}

} // namespace dish::composer
