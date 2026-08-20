// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What closing the window means when Dish is allowed to keep running, and
// whether the user has to be told. Pure: the shell performs, this decides.

#pragma once

#include <cstdint>

namespace dish::reducer {

enum class CloseAction : std::uint8_t { Quit, HideToBackground };

// Hiding with no tray host strands a running process behind no window and no
// menu, so the preference alone never decides.
inline CloseAction decideCloseAction(bool backgroundEnabled, bool trayAvailable) {
    if (backgroundEnabled && trayAvailable) { return CloseAction::HideToBackground; }
    return CloseAction::Quit;
}

// A window that vanishes without a word reads as a crash.
inline bool shouldAnnounceBackground(CloseAction action, bool alreadyAnnounced) {
    return action == CloseAction::HideToBackground && !alreadyAnnounced;
}

} // namespace dish::reducer
