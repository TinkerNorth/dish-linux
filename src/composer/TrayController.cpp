// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/TrayController.h"

#include "source/tray/TrayIcon.h"

namespace dish::composer {

TrayController::TrayController(const arch::Observable<reducer::TrayPresentation>& presentation,
                               source::TrayIcon* tray)
    : arch::Controller<reducer::TrayPresentation>(presentation), tray_(tray) {}

void TrayController::apply(const reducer::TrayPresentation& value) {
    if (stopped_ || tray_ == nullptr) { return; }
    tray_->setPresentation(value);
}

void TrayController::onStarting() {
    stopped_ = false;
    if (tray_ != nullptr) { tray_->show(); }
}

void TrayController::stop() {
    stopped_ = true;
    cancelCollection();
    if (tray_ != nullptr) { tray_->hide(); }
}

} // namespace dish::composer
