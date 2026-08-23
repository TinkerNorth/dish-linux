// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/CrashReportingBackend.h"

#include <QLoggingCategory>

namespace dish::composer {

namespace {
Q_LOGGING_CATEGORY(lcCrash, "dish.crash")
} // namespace

void LocalCrashReportingBackend::setEnabled(bool enabled) {
    // Logged only so a developer can confirm the opt-in plumbing fires. The
    // wording matters: the old "no backend wired" read as "this feature is
    // unfinished", when local-only collection IS the finished design.
    qCInfo(lcCrash) << "local crash logging" << (enabled ? "enabled" : "disabled")
                    << "- reports stay on this machine";
}

} // namespace dish::composer
