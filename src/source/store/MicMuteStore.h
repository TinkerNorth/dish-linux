// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Per-slot mic mute, the client-side half of the privacy invariant: while a
// slot is muted no MSG_MIC_AUDIO packet leaves this machine at all. Muting
// closes the capture delivery outright rather than sending silence, so
// "muted" is not a promise about the contents of a stream, it is the absence
// of one.
//
// One holder for every slot, because both mute controls mean the same thing
// and a user should not have to remember which one they used: the slot card's
// mute control writes here, and so does a Direct-claimed DualSense's own mute
// button (through the report decoder's latch, which folds the state onto the
// wire, and the manager's edge upcall, which mirrors it up).
//
// Deliberately NOT persisted, unlike the mic toggle beside it (mirrors
// dish-android's MicMuteStore). Mute is a live control over a session the way
// it is on the hardware, where it clears when the pad powers down; the
// durable "do not capture" answer is MicEnabledStore, which is off by default
// and survives restarts. A mute that outlived the process would also be
// invisible until the next session armed a microphone — every mute surface
// only shows while one is armed — which is the worst possible place to hide a
// microphone that looks broken. retainOnly() clears departed slots for the
// same reason: a replugged pad reuses its model-keyed slot id, and it must
// come back live like the hardware does.

#pragma once

#include "architecture/StateSource.h"

#include <map>
#include <set>
#include <string>

namespace dish::source {

using MicMuteMap = std::map<std::string, bool>;

class MicMuteStore : public arch::StateSource<MicMuteMap> {
  public:
    // Absent means unmuted: nothing has asked this slot to stop capturing.
    static constexpr bool kDefaultMuted = false;

    MicMuteStore() : arch::StateSource<MicMuteMap>(MicMuteMap{}) {}

    bool isMuted(const std::string& slotId) const;
    void setMuted(const std::string& slotId, bool muted);

    // What both mute controls do: the state is a toggle, the control is a
    // press. Returns the new state.
    bool toggle(const std::string& slotId);

    // Drop every slot not in `present`, so a departed pad's mute does not
    // resurrect on replug.
    void retainOnly(const std::set<std::string>& present);
};

} // namespace dish::source
