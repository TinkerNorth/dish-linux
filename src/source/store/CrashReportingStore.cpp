// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/CrashReportingStore.h"

#include <QSettings>

namespace dish::source {

bool CrashReportingStore::readInitial(QSettings& settings) {
    // Absent -> kDefaultEnabled (true): collection is opt-out.
    return settings.value(QLatin1String(kKeyCollectionEnabled), kDefaultEnabled).toBool();
}

CrashReportingStore::CrashReportingStore()
    : CrashReportingStore(
          std::make_unique<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {}

CrashReportingStore::CrashReportingStore(std::unique_ptr<QSettings> settings)
    : arch::StateSource<bool>(readInitial(*settings)), settings_(std::move(settings)) {}

CrashReportingStore::~CrashReportingStore() = default;

void CrashReportingStore::setEnabled(bool enabled) {
    settings_->setValue(QLatin1String(kKeyCollectionEnabled), enabled);
    setState(enabled);
}

} // namespace dish::source
