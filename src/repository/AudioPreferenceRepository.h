// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AudioPreferenceRepository — durable per-slot controller-audio toggles, stored
// as ONE JSON array under a caller-named key ("mic_preferences" /
// "speaker_preferences", so the two directions never share a blob). The shape
// and both invariants are MotionPreferenceRepository's:
//   * get() on a slot NEVER written returns std::nullopt, not a default
//     boolean. The store layer above turns absence into the default, so the
//     repo has to stay honest about "undecided" vs "explicitly enabled".
//   * Corrupt JSON falls back to an EMPTY list — losing toggles beats bricking
//     startup on a garbled blob.
// One parameterized class rather than two copies, because the mic and speaker
// rows differ ONLY in their settings key; the per-direction defaults live in
// the stores (source/store/MicEnabledStore.h, SpeakerEnabledStore.h).

#pragma once

#include "architecture/Repository.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

struct AudioPreference {
    QString slotId;
    bool enabled = false;

    bool operator==(const AudioPreference& o) const {
        return slotId == o.slotId && enabled == o.enabled;
    }
    bool operator!=(const AudioPreference& o) const { return !(*this == o); }
};

class AudioPreferenceRepository : public arch::KeyedRepository<QString, AudioPreference> {
  public:
    // `listKey` is the QSettings key the whole list lives under; it must be
    // unique per direction. nullptr settings -> the default HKCU store; tests
    // inject their own.
    explicit AudioPreferenceRepository(QString listKey,
                                       std::shared_ptr<QSettings> settings = nullptr);

    QString keyOf(const AudioPreference& value) const override { return value.slotId; }

    std::optional<AudioPreference> get(const QString& slotId) const override;
    std::vector<AudioPreference> all() const override;
    void put(const QString& slotId, const AudioPreference& value) override;
    void remove(const QString& slotId) override;
    void clear() override;

    // Un-hide the KeyedRepository value-overloads the declarations above shadow.
    using arch::KeyedRepository<QString, AudioPreference>::put;
    using arch::KeyedRepository<QString, AudioPreference>::removeValue;

  private:
    QString listKey_;
    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
