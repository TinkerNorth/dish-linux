// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Drives the tray item off the composed presentation. start() registers the
// item and applies the current value at once; stop() unregisters, so a quit
// never leaves a dead item behind on the panel.

#pragma once

#include "architecture/Controller.h"
#include "core/reducer/TrayPresentation.h"

namespace dish::source {
class TrayIcon;
}

namespace dish::composer {

class TrayController : public arch::Controller<reducer::TrayPresentation> {
  public:
    // `tray` is borrowed and may be nullptr in a headless build, in which case
    // this bookkeeps without effecting anything.
    TrayController(const arch::Observable<reducer::TrayPresentation>& presentation,
                   source::TrayIcon* tray);

  protected:
    void apply(const reducer::TrayPresentation& value) override;
    void onStarting() override;

  public:
    void stop() override;

  private:
    source::TrayIcon* tray_;
    bool stopped_ = false;
};

} // namespace dish::composer
