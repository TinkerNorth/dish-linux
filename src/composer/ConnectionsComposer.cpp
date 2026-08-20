// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/ConnectionsComposer.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace dish::composer {

namespace {

// First match wins: the desktop clients bind one physical pad per connection, and the
// Coordinator hands the vector over sorted, so this is deterministic.
std::string boundSlotFor(const std::string& connId, const std::vector<Binding>& bindings) {
    for (const auto& b : bindings) {
        if (b.connectionId == connId) { return b.slotId; }
    }
    return {};
}

} // namespace

std::vector<ConnectionRow> buildConnectionSummaries(
    const std::vector<SessionSnapshot>& sessions, const std::vector<RememberedSnapshot>& remembered,
    const std::vector<std::string>& discoveredIds, const std::vector<Binding>& bindings,
    const std::vector<std::string>& staleIds) {
    const std::set<std::string> discovered(discoveredIds.begin(), discoveredIds.end());
    const std::set<std::string> stale(staleIds.begin(), staleIds.end());

    std::unordered_map<std::string, const SessionSnapshot*> sessionById;
    for (const auto& s : sessions) { sessionById.emplace(s.id, &s); }
    std::unordered_map<std::string, const RememberedSnapshot*> rememberedById;
    for (const auto& r : remembered) { rememberedById.emplace(r.id, &r); }

    // (remembered ∪ live), so a session mid-pair (not yet remembered) and a
    // remembered-but-offline satellite each still get a row.
    std::set<std::string> ids;
    for (const auto& [id, s] : sessionById) { ids.insert(id); }
    for (const auto& [id, r] : rememberedById) { ids.insert(id); }

    std::vector<ConnectionRow> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        const SessionSnapshot* conn = nullptr;
        if (const auto it = sessionById.find(id); it != sessionById.end()) { conn = it->second; }
        const RememberedSnapshot* rem = nullptr;
        if (const auto it = rememberedById.find(id); it != rememberedById.end()) {
            rem = it->second;
        }

        // The live session's address may be fresher than the remembered row's.
        // With no ip on either the row is a ghost, and is dropped.
        std::string name;
        std::string ip;
        int udpPort = 0;
        if (conn != nullptr && !conn->ip.empty()) {
            name = conn->name;
            ip = conn->ip;
            udpPort = conn->udpPort;
        } else if (rem != nullptr && !rem->ip.empty()) {
            name = rem->name;
            ip = rem->ip;
            udpPort = rem->udpPort;
        } else {
            continue;
        }

        const reducer::SessionPresence presence =
            (conn != nullptr) ? conn->presence : reducer::SessionPresence::Idle;
        const reducer::UiLinkState live = reducer::satelliteLinkState(
            presence, /*isStale=*/stale.count(id) != 0, /*isDiscovered=*/discovered.count(id) != 0);

        ConnectionRow row;
        row.id = id;
        row.label = name.empty() ? ip : name;
        row.live = live;
        row.kind = reducer::ConnectionKind::Satellite;
        row.detailKey = reducer::RowDetailKey::DiscoveredRow;
        row.ip = ip;
        row.udpPort = udpPort;
        row.boundSlotId = boundSlotFor(id, bindings);
        if (conn != nullptr) {
            row.latencyOneWayMs = conn->latencyOneWayMs;
            row.latencySamples = conn->latencySamples;
        }
        row.glyph = reducer::glyphForConnection(row.kind, live);
        row.dotColor = reducer::dotColorForState(live);
        row.chip = reducer::statusChipKey(live);
        out.push_back(std::move(row));
    }

    // Stable order, so distinct-until-changed never re-emits on iteration order
    // alone. The id tiebreaks because two boxes can share a label.
    std::sort(out.begin(), out.end(), [](const ConnectionRow& a, const ConnectionRow& b) {
        if (a.label != b.label) { return a.label < b.label; }
        return a.id < b.id;
    });
    return out;
}

} // namespace dish::composer
