// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the tray item shows. The status is deliberately absent: a StatusNotifier
// host may hide a Passive item, and a hidden item is a running Dish the user can
// neither reach nor quit, so the gateway pins Active for as long as it is
// registered.

#pragma once

#include <cstdint>

namespace dish::reducer {

enum class TrayActivity : std::uint8_t { Idle, Streaming };

struct TrayPresentation {
    TrayActivity activity = TrayActivity::Idle;
    int streamingSlots = 0;
    bool windowVisible = true;

    bool operator==(const TrayPresentation& o) const {
        return activity == o.activity && streamingSlots == o.streamingSlots &&
               windowVisible == o.windowVisible;
    }
    bool operator!=(const TrayPresentation& o) const { return !(*this == o); }
};

inline TrayPresentation deriveTrayPresentation(bool windowVisible, int streamingSlots) {
    const int active = streamingSlots > 0 ? streamingSlots : 0;
    return TrayPresentation{active > 0 ? TrayActivity::Streaming : TrayActivity::Idle, active,
                            windowVisible};
}

} // namespace dish::reducer
