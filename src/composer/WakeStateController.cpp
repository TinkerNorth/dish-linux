// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/WakeStateController.h"

#include "Util/DisplaySleepInhibitor.h"

namespace dish::composer {

WakeStateController::WakeStateController(const arch::Observable<WakeState>& wakeState,
                                         util::DisplaySleepInhibitor* inhibitor, QString reason)
    : arch::Controller<WakeState>(wakeState), inhibitor_(inhibitor), reason_(std::move(reason)) {}

bool WakeStateController::isInhibiting() const {
    return inhibitor_ != nullptr && inhibitor_->isHeld();
}

void WakeStateController::apply(const WakeState& value) {
    // Belt-and-braces: the kernel already tore the subscription down on stop().
    if (stopped_) { return; }
    if (inhibitor_ == nullptr) { return; }
    // acquire/release are idempotent, so the OS is only touched on a real flip.
    if (value.shouldInhibit) {
        inhibitor_->acquire(reason_);
    } else {
        inhibitor_->release();
    }
}

void WakeStateController::onStarting() {
    // Re-arm so a restarted controller actuates again.
    stopped_ = false;
}

void WakeStateController::stop() {
    stopped_ = true;
    cancelCollection();
    // Release too, or the wake flag is stranded when streaming ends.
    if (inhibitor_ != nullptr) { inhibitor_->release(); }
}

} // namespace dish::composer
