// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Session presence to the UI LinkState chip a connection row renders. Owned here
// rather than in a composer so the dashboard and the connections screen agree by
// construction. Emits an enum, never localized text.

#pragma once

namespace dish::reducer {

// Mirrors dish::net::SessionState one-to-one, restated here so this header needs
// no Qt or Network dependency. The manager maps its enum on at the call boundary.
//   Idle      no live session, paired or not
//   Linking   session PUT / auth handshake in flight
//   Live      UDP socket open, heartbeat acks flowing
//   Faltering live, missing heartbeats, still below the death threshold
//   Stale     the session collapsed and the key was dropped
enum class SessionPresence { Idle, Linking, Live, Faltering, Stale };

// Mirrors dish::models::LinkState. The UI turns this into a localized chip.
enum class UiLinkState { Found, Stale, Saved, Ready, Connecting, Connected, Unstable };

// A live or linking session ignores both flags: presence wins. `isStale` is the
// out-of-band "Needs pairing" marker set on a terminal 401 or unpaired close, and
// the manager sets it alongside a Stale presence, so both paths land there.
inline UiLinkState satelliteLinkState(SessionPresence presence, bool isStale, bool isDiscovered) {
    switch (presence) {
    case SessionPresence::Live:
        return UiLinkState::Connected;
    case SessionPresence::Linking:
        return UiLinkState::Connecting;
    case SessionPresence::Faltering:
        return UiLinkState::Unstable;
    case SessionPresence::Stale:
        return UiLinkState::Stale;
    case SessionPresence::Idle:
    default:
        if (isStale) { return UiLinkState::Stale; }
        if (isDiscovered) { return UiLinkState::Ready; }
        return UiLinkState::Saved;
    }
}

} // namespace dish::reducer
