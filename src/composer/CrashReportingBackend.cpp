// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/CrashReportingBackend.h"

#include <QLoggingCategory>

namespace dish::composer {

namespace {
Q_LOGGING_CATEGORY(lcCrash, "dish.crash")
} // namespace

void NoopCrashReportingBackend::setEnabled(bool enabled) {
    // Logged only so a developer can confirm the opt-in plumbing fires.
    qCInfo(lcCrash) << "crash-reporting collection set to" << enabled << "(no backend wired)";
}

} // namespace dish::composer
