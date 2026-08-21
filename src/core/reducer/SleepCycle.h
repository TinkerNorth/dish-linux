// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The suspend/resume edge, as a two-state machine. logind repeats
// PrepareForSleep(true) when a suspend escalates to hibernate, and a resume can
// arrive for a suspend Dish never saw, so every repeat and every unpaired edge
// has to be inert.

#pragma once

#include <cstdint>

namespace dish::reducer {

enum class SleepPhase : std::uint8_t { Awake, Suspending };
enum class SleepEffect : std::uint8_t { None, TearDown, Reconnect };

struct SleepReduction {
    SleepPhase next = SleepPhase::Awake;
    SleepEffect effect = SleepEffect::None;

    bool operator==(const SleepReduction& o) const { return next == o.next && effect == o.effect; }
    bool operator!=(const SleepReduction& o) const { return !(*this == o); }
};

inline SleepReduction reduceSleepCycle(SleepPhase current, bool preparingForSleep) {
    switch (current) {
    case SleepPhase::Awake:
        if (preparingForSleep) {
            return SleepReduction{SleepPhase::Suspending, SleepEffect::TearDown};
        }
        return SleepReduction{SleepPhase::Awake, SleepEffect::None};
    case SleepPhase::Suspending:
        if (preparingForSleep) { return SleepReduction{SleepPhase::Suspending, SleepEffect::None}; }
        return SleepReduction{SleepPhase::Awake, SleepEffect::Reconnect};
    }
    return SleepReduction{current, SleepEffect::None};
}

} // namespace dish::reducer
