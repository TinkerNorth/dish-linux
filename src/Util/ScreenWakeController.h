// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "DisplaySleepInhibitor.h"
#include "Models/Models.h"

#include <QHash>
#include <QString>

namespace dish::util {

// Owns the "are we streaming?" count and drives a DisplaySleepInhibitor off
// it. Mirrors dish-android :: WakeStateController and dish-mac ::
// ScreenWakeController — the inhibitor flips on the 0↔positive transition,
// same-count emissions are no-ops so a noisy hub feed doesn't thrash D-Bus.
//
// Pure logic, no Qt signal subscription. The AppModel chooses when to call
// update(...); the controller decides whether to call inhibitor.acquire /
// release. Lets tests pin the transition contract via FakeDisplaySleepInhibitor.
class ScreenWakeController {
  public:
    explicit ScreenWakeController(
        DisplaySleepInhibitor* inhibitor,
        QString reason = QStringLiteral("Dish is streaming gamepad input to Satellite"));

    int streamingSlotCount() const { return streamingSlotCount_; }

    // Pure helper that derives the count of bound + online slots from the
    // current binding table and the per-connection link state. Extracted so
    // unit tests can pin the arithmetic without instantiating a controller.
    static int streamingCount(const QHash<QString, QString>& bindings,
                              const QHash<QString, models::LinkState>& connectionStates);

    // Feed the controller a fresh streaming count. Acquires the inhibitor on
    // the 0 → positive transition; releases on positive → 0. Same value twice
    // is a no-op so callers can spam updates without thrash.
    void update(int streamingSlotCount);

    // Backgrounding / shutdown path. Drops the inhibitor unconditionally and
    // resets the count so the next update re-establishes from scratch.
    void reset();

  private:
    DisplaySleepInhibitor* inhibitor_;
    QString reason_;
    int streamingSlotCount_ = 0;
};

} // namespace dish::util
