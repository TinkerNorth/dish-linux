// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MotionPreferenceRepository — durable per-slot motion-enable toggles, stored as
// ONE JSON array under the "motion_preferences" key. Wrap in MotionEnabledStore
// for reactive reads.
//
// Two invariants:
//   * get() on a slot NEVER written returns std::nullopt, not a default boolean.
//     The store layer above turns absence into the default, so the repo has to
//     stay honest about "undecided" vs "explicitly enabled".
//   * Corrupt JSON falls back to an EMPTY list — losing toggles beats bricking
//     startup on a garbled blob.

#pragma once

#include "architecture/Repository.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

struct MotionPreference {
    QString slotId;
    bool enabled = false;

    bool operator==(const MotionPreference& o) const {
        return slotId == o.slotId && enabled == o.enabled;
    }
    bool operator!=(const MotionPreference& o) const { return !(*this == o); }
};

class MotionPreferenceRepository : public arch::KeyedRepository<QString, MotionPreference> {
  public:
    // nullptr -> the default HKCU store; tests inject their own.
    explicit MotionPreferenceRepository(std::shared_ptr<QSettings> settings = nullptr);

    QString keyOf(const MotionPreference& value) const override { return value.slotId; }

    std::optional<MotionPreference> get(const QString& slotId) const override;
    std::vector<MotionPreference> all() const override;
    void put(const QString& slotId, const MotionPreference& value) override;
    void remove(const QString& slotId) override;
    void clear() override;

    // Un-hide the KeyedRepository value-overloads the declarations above shadow.
    using arch::KeyedRepository<QString, MotionPreference>::put;
    using arch::KeyedRepository<QString, MotionPreference>::removeValue;

  private:
    // Both assume mutex_ is held.
    std::vector<MotionPreference> readList() const;
    void writeList(const std::vector<MotionPreference>& list);

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
