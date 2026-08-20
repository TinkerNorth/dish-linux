// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Derives the flat connections list the UI renders from five upstream snapshots
// (session presence, discovered ids, remembered satellites, bindings, stale
// ids). Everything stays Qt-free and unlocalized — row text is carried as a
// render key plus args, so localization happens at the UI edge only.

#pragma once

#include "architecture/Composer.h"
#include "architecture/Observable.h"
#include "core/reducer/ConnectionRows.h"
#include "core/reducer/SatelliteLinkState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dish::composer {

// ── Upstream snapshot value types (Qt-free, copyable, == comparable) ─────────

struct SessionSnapshot {
    std::string id;
    reducer::SessionPresence presence = reducer::SessionPresence::Idle;
    std::string name; // server display name (may be empty)
    std::string ip;
    int udpPort = 0;
    // Median heartbeat-RTT/2 in ms, already display-rounded by the WifiConnection
    // poll — so exact == here keys distinct-until-changed on display moves.
    double latencyOneWayMs = 0.0;
    int latencySamples = 0;

    bool operator==(const SessionSnapshot& o) const {
        return id == o.id && presence == o.presence && name == o.name && ip == o.ip &&
               udpPort == o.udpPort && latencyOneWayMs == o.latencyOneWayMs &&
               latencySamples == o.latencySamples;
    }
    bool operator!=(const SessionSnapshot& o) const { return !(*this == o); }
};

// A remembered entry with no live session still renders (Offline/Stale/Ready).
struct RememberedSnapshot {
    std::string id;
    std::string name;
    std::string ip;
    int udpPort = 0;

    bool operator==(const RememberedSnapshot& o) const {
        return id == o.id && name == o.name && ip == o.ip && udpPort == o.udpPort;
    }
    bool operator!=(const RememberedSnapshot& o) const { return !(*this == o); }
};

// ── The derived row (the composer's output element) ──────────────────────────

struct ConnectionRow {
    std::string id;
    std::string label; // server name, or ip when the name is empty
    reducer::UiLinkState live = reducer::UiLinkState::Saved;
    reducer::ConnectionKind kind = reducer::ConnectionKind::Satellite;
    // Detail line as data, not localized text (UI formats "<ip> • UDP <port>").
    reducer::RowDetailKey detailKey = reducer::RowDetailKey::DiscoveredRow;
    std::string ip;
    int udpPort = 0;
    // Empty = unbound. One slot id suffices: this client binds one physical pad per
    // connection.
    std::string boundSlotId;
    // Pre-mapped render keys, so the UI never re-derives them and can't drift.
    reducer::ConnectionGlyph glyph = reducer::ConnectionGlyph::SatelliteBase;
    reducer::DotColor dotColor = reducer::DotColor::Muted;
    reducer::StatusChipKey chip = reducer::StatusChipKey::Offline;
    // 0 / 0 for a remembered-only row.
    double latencyOneWayMs = 0.0;
    int latencySamples = 0;

    bool operator==(const ConnectionRow& o) const {
        return id == o.id && label == o.label && live == o.live && kind == o.kind &&
               detailKey == o.detailKey && ip == o.ip && udpPort == o.udpPort &&
               boundSlotId == o.boundSlotId && glyph == o.glyph && dotColor == o.dotColor &&
               chip == o.chip && latencyOneWayMs == o.latencyOneWayMs &&
               latencySamples == o.latencySamples;
    }
    bool operator!=(const ConnectionRow& o) const { return !(*this == o); }
};

// One slotId -> connectionId binding.
struct Binding {
    std::string slotId;
    std::string connectionId;

    bool operator==(const Binding& o) const {
        return slotId == o.slotId && connectionId == o.connectionId;
    }
};

// One row per id in (remembered ∪ live), sorted by label so the output is stable
// enough for distinct-until-changed. Pure.
std::vector<ConnectionRow> buildConnectionSummaries(
    const std::vector<SessionSnapshot>& sessions, const std::vector<RememberedSnapshot>& remembered,
    const std::vector<std::string>& discoveredIds, const std::vector<Binding>& bindings,
    const std::vector<std::string>& staleIds);

// ── The Composer (combines the 5 upstreams via the pure transform) ───────────

// The upstream Observables are owned by the Coordinator; this only reads them.
class ConnectionsComposer
    : public arch::Composer<std::vector<ConnectionRow>, std::vector<SessionSnapshot>,
                            std::vector<RememberedSnapshot>, std::vector<std::string>,
                            std::vector<Binding>, std::vector<std::string>> {
  public:
    ConnectionsComposer(const arch::Observable<std::vector<SessionSnapshot>>& sessions,
                        const arch::Observable<std::vector<RememberedSnapshot>>& remembered,
                        const arch::Observable<std::vector<std::string>>& discoveredIds,
                        const arch::Observable<std::vector<Binding>>& bindings,
                        const arch::Observable<std::vector<std::string>>& staleIds)
        : arch::Composer<std::vector<ConnectionRow>, std::vector<SessionSnapshot>,
                         std::vector<RememberedSnapshot>, std::vector<std::string>,
                         std::vector<Binding>, std::vector<std::string>>(
              sessions, remembered, discoveredIds, bindings, staleIds,
              [](const std::vector<SessionSnapshot>& s, const std::vector<RememberedSnapshot>& r,
                 const std::vector<std::string>& d, const std::vector<Binding>& b,
                 const std::vector<std::string>& st) {
                  return buildConnectionSummaries(s, r, d, b, st);
              }) {}
};

} // namespace dish::composer
