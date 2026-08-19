// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Connection-row shaping: glyph, dot colour, status chip and liveness from a
// UiLinkState. Everything is emitted as keys and args, never localized text, so
// the composer layer never reaches for tr(); the UI resolves the keys.

#pragma once

#include "core/reducer/SatelliteLinkState.h"

namespace dish::reducer {

// Only Satellite is wired: this client is physical-controllers-only.
enum class ConnectionKind { Satellite };

// Icon keys the UI resolves to Qt resources.
enum class ConnectionGlyph { SatelliteBase, SatelliteConnected, SatelliteOff };

// Semantic colour tokens the UI resolves against the theme.
enum class DotColor { Success, Primary, Warning, Muted };

// One key per LinkState, the chip vocabulary shared across the Dish clients.
enum class StatusChipKey { Found, NeedsPairing, Offline, Ready, Connecting, Online, Unstable };

// The UI formats the detail line from the (ip, udpPort) args under its own
// catalogue, which is what keeps this layer free of tr().
enum class RowDetailKey { DiscoveredRow };

// An Unstable link is still streaming, so the dashboard and the connections
// screen both count it as live.
inline bool isLiveLink(UiLinkState state) {
    return state == UiLinkState::Connected || state == UiLinkState::Unstable;
}

inline ConnectionGlyph glyphForConnection(ConnectionKind kind, UiLinkState state) {
    switch (kind) {
    case ConnectionKind::Satellite:
    default:
        switch (state) {
        case UiLinkState::Connected:
            return ConnectionGlyph::SatelliteConnected;
        case UiLinkState::Saved:
        case UiLinkState::Stale:
            return ConnectionGlyph::SatelliteOff;
        default:
            return ConnectionGlyph::SatelliteBase;
        }
    }
}

inline DotColor dotColorForState(UiLinkState state) {
    switch (state) {
    case UiLinkState::Connected:
        return DotColor::Success;
    case UiLinkState::Connecting:
    case UiLinkState::Unstable:
        return DotColor::Primary;
    case UiLinkState::Stale:
        return DotColor::Warning;
    case UiLinkState::Found:
    case UiLinkState::Saved:
    case UiLinkState::Ready:
    default:
        return DotColor::Muted;
    }
}

inline StatusChipKey statusChipKey(UiLinkState state) {
    switch (state) {
    case UiLinkState::Found:
        return StatusChipKey::Found;
    case UiLinkState::Stale:
        return StatusChipKey::NeedsPairing;
    case UiLinkState::Saved:
        return StatusChipKey::Offline;
    case UiLinkState::Ready:
        return StatusChipKey::Ready;
    case UiLinkState::Connecting:
        return StatusChipKey::Connecting;
    case UiLinkState::Connected:
        return StatusChipKey::Online;
    case UiLinkState::Unstable:
    default:
        return StatusChipKey::Unstable;
    }
}

} // namespace dish::reducer
