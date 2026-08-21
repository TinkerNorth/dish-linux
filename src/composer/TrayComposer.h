// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the tray item should be showing right now. The derive half; TrayController
// owns the effect.

#pragma once

#include "architecture/Composer.h"
#include "architecture/Observable.h"
#include "core/reducer/TrayPresentation.h"

namespace dish::composer {

class TrayComposer : public arch::Composer<reducer::TrayPresentation, bool, int> {
  public:
    TrayComposer(const arch::Observable<bool>& windowVisible,
                 const arch::Observable<int>& streamingSlotCount)
        : arch::Composer<reducer::TrayPresentation, bool, int>(windowVisible, streamingSlotCount,
                                                               reducer::deriveTrayPresentation) {}
};

} // namespace dish::composer
