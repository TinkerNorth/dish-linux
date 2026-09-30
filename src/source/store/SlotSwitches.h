// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The four per-slot switches a binding page writes (motion, rumble, microphone,
// speaker), as one bundle. They share a key, the slot id, so whatever moves a
// binding from one slot id to another moves them together. Mirrors dish-android
// source/store/SlotToggleStores.

#pragma once

#include "source/store/AudioEnabledStore.h"
#include "source/store/MotionEnabledStore.h"
#include "source/store/RumbleEnabledStore.h"

#include <string>

namespace dish::source {

struct SlotSwitchStores {
    MotionEnabledStore& motion;
    RumbleEnabledStore& rumble;
    MicEnabledStore& mic;
    SpeakerEnabledStore& speaker;
};

// A Direct claim or release gives the same pad a new slot id, and the binding
// migrates to it (reducer::resolveBindingPresence). The new id then answers every
// switch exactly as the old one did: an explicit choice is copied, and a switch
// never touched on the old id leaves the new one at its default too, whatever it
// held from an earlier claim.
void carrySlotSwitches(const SlotSwitchStores& stores, const std::string& fromSlotId,
                       const std::string& toSlotId);

} // namespace dish::source
