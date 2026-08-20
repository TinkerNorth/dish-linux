// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// CrashReportingStore — owns the persisted crash-reporting "collection enabled"
// flag and republishes it reactively.
//
// This is an opt-OUT toggle: kDefaultEnabled is true, so a fresh store reads
// true and flipping the toggle turns collection OFF. Do not invert it — the
// default is deliberate and matches the Android client.
//
// The persisted key is kept verbatim from the Android client for cross-client
// schema continuity; renaming it is a migration.

#pragma once

#include "architecture/StateSource.h"

#include <memory>

class QSettings;

namespace dish::source {

class CrashReportingStore : public arch::StateSource<bool> {
  public:
    static constexpr const char* kKeyCollectionEnabled = "crashlytics_collection_enabled";
    static constexpr bool kDefaultEnabled = true;

    // Production ctor: a QSettings under the app org. Test ctor: inject a store.
    CrashReportingStore();
    explicit CrashReportingStore(std::unique_ptr<QSettings> settings);
    ~CrashReportingStore() override;

    bool enabled() const { return state().value(); }

    // Persist + republish. Distinct-until-changed: a redundant set does not
    // re-emit.
    void setEnabled(bool enabled);

  private:
    static bool readInitial(QSettings& settings);

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
