// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// ControllerTypeStore — an in-memory StateSource over the user's per-slot
// "Emulate" controller-type override: (connectionId, slotId) -> type. The
// Emulate picker writes here; the connection layer reads it when building the
// per-controller descriptor (it threads the chosen type into the session PUT).
// Owns no IO and no durable storage — it is the live override map, defaulting to
// empty (no override = fall back to the SDL hardware classification). Mirrors
// dish-android source/store/ControllerTypeStore (an AbstractStateSource over
// Map<Pair<String,String>, Int>) with setType / setTypeIfAbsent / clear /
// clearConnection / typeFor / slotTypesFor.

#pragma once

#include "architecture/StateSource.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dish::source {

// (connectionId, slotId) -> controllerType. std::map gives a deterministic,
// ==-comparable value so the Observable's distinct-until-changed suppresses
// no-op re-emits (a setTypeIfAbsent that changes nothing emits nothing).
using ControllerTypeMap = std::map<std::pair<std::string, std::string>, int>;

class ControllerTypeStore : public arch::StateSource<ControllerTypeMap> {
  public:
    ControllerTypeStore() : arch::StateSource<ControllerTypeMap>(ControllerTypeMap{}) {}

    // The override type for a (connection, slot), or nullopt when unset. nullopt
    // is the caller's cue to fall back to the hardware-classified type.
    std::optional<int> typeFor(const std::string& connectionId, const std::string& slotId) const {
        const auto& snapshot = state().value();
        const auto it = snapshot.find({connectionId, slotId});
        if (it == snapshot.end()) { return std::nullopt; }
        return it->second;
    }

    // Set (overwrite) the override for a (connection, slot).
    void setType(const std::string& connectionId, const std::string& slotId, int type) {
        setState([&](const ControllerTypeMap& current) {
            ControllerTypeMap next = current;
            next[{connectionId, slotId}] = type;
            return next;
        });
    }

    // Set the override only if one is not already present — used to seed a
    // remembered/default type without stomping a user choice.
    void setTypeIfAbsent(const std::string& connectionId, const std::string& slotId, int type) {
        setState([&](const ControllerTypeMap& current) {
            const std::pair<std::string, std::string> key{connectionId, slotId};
            if (current.find(key) != current.end()) { return current; }
            ControllerTypeMap next = current;
            next[key] = type;
            return next;
        });
    }

    // Drop the override for exactly one (connection, slot). A no-op (no emit) if
    // it was unset; sibling slots of the same connection survive.
    void clear(const std::string& connectionId, const std::string& slotId) {
        setState([&](const ControllerTypeMap& current) {
            const std::pair<std::string, std::string> key{connectionId, slotId};
            if (current.find(key) == current.end()) { return current; }
            ControllerTypeMap next = current;
            next.erase(key);
            return next;
        });
    }

    // Drop every override under a connection at once (e.g. when it is forgotten),
    // leaving other connections untouched.
    void clearConnection(const std::string& connectionId) {
        setState([&](const ControllerTypeMap& current) {
            ControllerTypeMap next;
            for (const auto& [key, type] : current) {
                if (key.first != connectionId) { next.emplace(key, type); }
            }
            return next;
        });
    }

    // The subset of `boundSlotIds` under `connectionId` that carry an override,
    // as slotId -> type. Slots with no override are omitted (the caller falls
    // back to the hardware type for them). Non-inline (see .cpp).
    std::map<std::string, int> slotTypesFor(const std::string& connectionId,
                                            const std::vector<std::string>& boundSlotIds) const;
};

} // namespace dish::source
