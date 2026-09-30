// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// RumblePreferenceRepository: durable per-slot rumble switches, stored as ONE
// JSON array under the "rumble_preferences" key in the row shape
// MotionPreferenceRepository writes. Wrap in RumbleEnabledStore for reactive
// reads.
//
// Two invariants:
//   * get() on a slot NEVER written returns std::nullopt, not a default
//     boolean. The store layer above turns absence into the default, so the
//     repo has to stay honest about "never switched" vs "explicitly on".
//   * A garbled blob reads as an EMPTY list: losing the switches beats
//     bricking startup.

#pragma once

#include "architecture/Repository.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dish::repository {

struct RumblePreference {
    QString slotId;
    bool enabled = false;

    bool operator==(const RumblePreference&) const = default;
};

class RumblePreferenceRepository : public arch::KeyedRepository<QString, RumblePreference> {
  public:
    // The app's own config file (repository::makeSettings).
    RumblePreferenceRepository();
    // Tests hand in a throwaway store.
    explicit RumblePreferenceRepository(std::shared_ptr<QSettings> settings);

    QString keyOf(const RumblePreference& value) const override { return value.slotId; }

    std::optional<RumblePreference> get(const QString& slotId) const override;
    std::vector<RumblePreference> all() const override;
    void put(const QString& slotId, const RumblePreference& value) override;
    void remove(const QString& slotId) override;
    void clear() override;

    // Un-hide the KeyedRepository value overload the declaration above shadows.
    using arch::KeyedRepository<QString, RumblePreference>::put;

  private:
    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
