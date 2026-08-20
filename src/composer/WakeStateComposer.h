// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Should Dish keep the display awake right now? The derive half of the wake
// subsystem; WakeStateController owns the effect. WakeState is ==-comparable, so
// distinct-until-changed gives the 0<->positive no-thrash contract for free.

#pragma once

#include "architecture/Composer.h"
#include "architecture/Observable.h"

namespace dish::composer {

// The controller acts on `shouldInhibit` alone; `streamingSlotCount` rides along
// so a count change that doesn't flip the bool still reads as a distinct value.
struct WakeState {
    bool shouldInhibit = false;
    int streamingSlotCount = 0;

    bool operator==(const WakeState& o) const {
        return shouldInhibit == o.shouldInhibit && streamingSlotCount == o.streamingSlotCount;
    }
    bool operator!=(const WakeState& o) const { return !(*this == o); }
};

// Awake iff at least one slot is streaming, or an override is in force. Pure.
inline WakeState deriveWakeState(int streamingSlotCount, int shouldKeepScreenOn) {
    const bool inhibit = streamingSlotCount > 0 || shouldKeepScreenOn > 0;
    return WakeState{inhibit, streamingSlotCount};
}

class WakeStateComposer : public arch::Composer<WakeState, int, int> {
  public:
    WakeStateComposer(const arch::Observable<int>& streamingSlotCount,
                      const arch::Observable<int>& shouldKeepScreenOn)
        : arch::Composer<WakeState, int, int>(streamingSlotCount, shouldKeepScreenOn,
                                              deriveWakeState) {}
};

} // namespace dish::composer
