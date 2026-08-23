// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// How far Dish should hold the machine awake right now. The derive half of the
// wake subsystem; WakeStateController owns the effect. WakeState is
// ==-comparable, so distinct-until-changed gives the no-thrash contract for
// free.

#pragma once

#include "architecture/Composer.h"
#include "architecture/Observable.h"
#include "core/reducer/KeepAwake.h"

namespace dish::composer {

// The controller acts on `reach` alone; `streamingSlotCount` rides along so a
// count change that doesn't move the reach still reads as a distinct value.
struct WakeState {
    reducer::KeepAwakeReach reach = reducer::KeepAwakeReach::None;
    int streamingSlotCount = 0;

    bool operator==(const WakeState& o) const {
        return reach == o.reach && streamingSlotCount == o.streamingSlotCount;
    }
    bool operator!=(const WakeState& o) const { return !(*this == o); }
};

inline WakeState deriveWakeState(int streamingSlotCount, bool controllerActive,
                                 const reducer::KeepAwakePreferences& prefs) {
    return WakeState{reducer::deriveKeepAwakeReach(prefs, streamingSlotCount, controllerActive),
                     streamingSlotCount};
}

class WakeStateComposer
    : public arch::Composer<WakeState, int, bool, reducer::KeepAwakePreferences> {
  public:
    WakeStateComposer(const arch::Observable<int>& streamingSlotCount,
                      const arch::Observable<bool>& controllerActive,
                      const arch::Observable<reducer::KeepAwakePreferences>& preferences)
        : arch::Composer<WakeState, int, bool, reducer::KeepAwakePreferences>(
              streamingSlotCount, controllerActive, preferences, deriveWakeState) {}
};

} // namespace dish::composer
