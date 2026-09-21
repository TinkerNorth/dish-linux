// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the app-wide microphone surface (the chip in the shell header, on every
// page) says about the microphone. Three states, because that is all a user
// needs to account for a microphone: there isn't one, there is one and it is
// hot, there is one and it is silenced. The sibling of dish-android's
// MicIndicatorPolicy, folded to the same three answers.
//
// The inputs are counts over the bound slots, both derived from the same
// AudioSlotFacts the capture engine's eligibility reads (core/audio/
// AudioEnginePolicy.h), so this can never disagree with what the engine is
// doing:
//
//   armed      streaming AND toggled on AND routed AND welcome at the host,
//              mute NOT considered: a muted slot still has a microphone the
//              user must be able to find and unmute;
//   capturing  armed AND not muted: exactly micCaptureEligible, the slots
//              whose capture device is open right now.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dish::reducer {

enum class MicIndicatorState : std::uint8_t {
    Hidden, // no armed microphone anywhere: nothing to account for, no surface
    Live,   // at least one armed slot is delivering: the microphone is hot
    Muted,  // armed, and every armed slot is muted: it exists and none is sent
};

inline MicIndicatorState micIndicatorFor(int armedSlots, int capturingSlots) {
    if (armedSlots <= 0) { return MicIndicatorState::Hidden; }
    if (capturingSlots > 0) { return MicIndicatorState::Live; }
    return MicIndicatorState::Muted;
}

// Every armed slot's mute, set to one value: what one click on the app-wide
// surface does. All-or-nothing on purpose: the surface shows ONE state for the
// whole machine, so its control must leave the machine in one state. A click
// on Live (even a mixed live-and-muted set) mutes everything; the next click
// unmutes everything.
struct MicMuteAllOrder {
    std::vector<std::string> slotIds;
    bool muted = false;
};

// Nullopt when nothing is armed: the surface is hidden then, and a stale click
// (a chip racing a session teardown) must not write mutes for slots that no
// longer capture.
inline std::optional<MicMuteAllOrder> micToggleAllFor(std::vector<std::string> armedSlotIds,
                                                      int capturingSlots) {
    if (armedSlotIds.empty()) { return std::nullopt; }
    MicMuteAllOrder order;
    order.slotIds = std::move(armedSlotIds);
    order.muted = capturingSlots > 0;
    return order;
}

} // namespace dish::reducer
