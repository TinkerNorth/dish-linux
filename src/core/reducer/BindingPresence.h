// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which bindings still have a physical pad behind them. A binding is a
// declaration, not a preference: every session PUT re-sends the whole desired set
// and the satellite creates a virtual pad per entry, so a binding left standing
// after its pad went away re-plugs a controller backed by no hardware.
//
//   pad present           keep
//   pad moved to its twin migrate. A USB-direct claim retires the framework slot
//                         id and publishes a synthetic in its place, and a release
//                         reverses that. Tearing the binding down instead would
//                         kill a working stream on every path switch.
//   pad gone              unbind, which DELETEs the controller on a live session
//                         rather than waiting for a server-side reaper timeout.
//
// Identity is the pad's USB (vid, pid); 0:0 is identity-less and matches nothing.

#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dish::reducer {

struct PresentSlot {
    std::string id;
    int vendorId = 0;
    int productId = 0;
};

// `identity` is the pad the binding was made against, when it can still be
// resolved: a framework twin hidden by an active claim is still enumerated, and a
// synthetic slot id parses back to its vid/pid. nullopt means the pad is gone.
struct BoundSlot {
    std::string slotId;
    std::string connId;
    std::optional<std::pair<int, int>> identity;
};

enum class BindingPresenceKind { Unbind, Migrate };

// `slotId` is always the binding to drop and `connId` the connection it pointed
// at: on a Migrate that is what to re-bind `toSlotId` to, on an Unbind it is who
// to name in the notice, since a binding that vanishes silently reads as a bug.
struct BindingPresenceAction {
    BindingPresenceKind kind = BindingPresenceKind::Unbind;
    std::string slotId;
    std::string toSlotId;
    std::string connId;
};

inline bool isPadIdentity(const std::optional<std::pair<int, int>>& identity) {
    return identity.has_value() && !(identity->first == 0 && identity->second == 0);
}

// Also the seam the emulation-type seed reads: `emulates` hints match against the
// pad's real identity, so a call site that cannot answer this silently degrades
// every pad to the catalog's first offered type.
inline std::optional<std::pair<int, int>> padIdentityFor(const std::string& slotId,
                                                         const std::vector<PresentSlot>& present) {
    for (const auto& s : present) {
        if (s.id != slotId) { continue; }
        const auto identity = std::make_optional(std::make_pair(s.vendorId, s.productId));
        return isPadIdentity(identity) ? identity : std::nullopt;
    }
    return std::nullopt;
}

// Ordered by the departing slot id, so the result is deterministic for tests and
// replay. Idempotent: re-running over the shape its own actions produce yields
// nothing, which is what lets AppModel re-enter rebuild after applying them.
inline std::vector<BindingPresenceAction>
resolveBindingPresence(const std::vector<PresentSlot>& present,
                       const std::vector<BoundSlot>& bindings) {
    std::map<std::string, const PresentSlot*> presentById;
    for (const auto& s : present) { presentById.emplace(s.id, &s); }

    // Bound slot ids, so a migration never steals a target that already carries
    // a binding of its own (the same model plugged twice).
    std::map<std::string, bool> boundIds;
    for (const auto& b : bindings) { boundIds.emplace(b.slotId, true); }

    // Keying the walk by slot id gives the deterministic order with no post-sort.
    std::map<std::string, const BoundSlot*> bySlotId;
    for (const auto& b : bindings) { bySlotId.emplace(b.slotId, &b); }

    std::vector<BindingPresenceAction> out;
    for (const auto& [slotId, binding] : bySlotId) {
        if (presentById.count(slotId) != 0) { continue; } // the pad is right there

        BindingPresenceAction action;
        action.slotId = slotId;
        action.connId = binding->connId;
        action.kind = BindingPresenceKind::Unbind;

        if (isPadIdentity(binding->identity)) {
            // Same pad, different slot id: the twin transition. First match in
            // the caller's slot order wins.
            for (const auto& candidate : present) {
                if (candidate.vendorId != binding->identity->first ||
                    candidate.productId != binding->identity->second) {
                    continue;
                }
                if (boundIds.count(candidate.id) != 0) { continue; }
                action.kind = BindingPresenceKind::Migrate;
                action.toSlotId = candidate.id;
                break;
            }
        }
        out.push_back(std::move(action));
    }
    return out;
}

} // namespace dish::reducer
