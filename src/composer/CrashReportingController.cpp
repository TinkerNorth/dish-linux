// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/CrashReportingController.h"

namespace dish::composer {

CrashReportingController::CrashReportingController(const arch::Observable<bool>& enabled,
                                                   CrashReportingBackend* backend)
    : arch::Controller<bool>(enabled), backend_(backend) {}

void CrashReportingController::apply(const bool& enabled) {
    if (backend_ != nullptr) { backend_->setEnabled(enabled); }
}

void CrashReportingController::stop() {
    // Deliberate no-op — do NOT call cancelCollection(). Process-scoped: the
    // opt-in must survive teardown and keep propagating flips to the backend.
}

} // namespace dish::composer
