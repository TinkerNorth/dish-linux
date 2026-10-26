// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A WakeInhibitor that is absolute and idempotent, like the production
// FreedesktopWakeInhibitor, and never touches a bus. `changes()` counts only
// the applies that actually moved the held hold, so a "does not re-apply"
// assertion still means something when every emission reaches apply().
// `refuseLid` stands in for a logind or polkit that says no to the lid lock.

#pragma once

#include "source/system/WakeInhibitor.h"

#include <QString>

namespace dish::test {

class FakeWakeInhibitor : public dish::source::WakeInhibitor {
  public:
    bool refuseLid = false;

    void apply(const dish::reducer::KeepAwakeHold& hold, const QString& reason) override {
        ++applies_;
        lastReason_ = reason;
        const bool lidGranted = hold.lid && !refuseLid;
        const dish::reducer::KeepAwakeHold achieved{hold.reach, lidGranted};
        if (achieved == held_) { return; }
        held_ = achieved;
        ++changes_;
    }
    dish::reducer::KeepAwakeHold held() const override { return held_; }

    int applies() const { return applies_; }
    int changes() const { return changes_; }
    QString lastReason() const { return lastReason_; }
    bool isHeld() const { return held_.reach != dish::reducer::KeepAwakeReach::None; }

  private:
    int applies_ = 0;
    int changes_ = 0;
    dish::reducer::KeepAwakeHold held_;
    QString lastReason_;
};

} // namespace dish::test
