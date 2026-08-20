// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Drives the OS display-sleep inhibitor (SetThreadExecutionState) off the
// composed WakeState. start() applies the current value immediately, so a
// controller started mid-stream acquires at once; stop() also RELEASES, so the
// wake flag is never stranded when streaming ends.

#pragma once

#include "architecture/Controller.h"
#include "composer/WakeStateComposer.h"

#include <QString>

namespace dish::util {
class DisplaySleepInhibitor;
}

namespace dish::composer {

class WakeStateController : public arch::Controller<WakeState> {
  public:
    // `inhibitor` is borrowed and may be nullptr in a headless build, in which
    // case this bookkeeps without effecting anything.
    WakeStateController(
        const arch::Observable<WakeState>& wakeState, util::DisplaySleepInhibitor* inhibitor,
        QString reason = QStringLiteral("Dish is streaming gamepad input to Satellite"));

    bool isInhibiting() const;

  protected:
    void apply(const WakeState& value) override;
    void onStarting() override;

  public:
    void stop() override;

  private:
    util::DisplaySleepInhibitor* inhibitor_;
    QString reason_;
    bool stopped_ = false;
};

} // namespace dish::composer
