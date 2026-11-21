// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The database is read at attach (docs/controller-db.md section 6.5): each
// pad keeps the record it was matched to, and the expect mismatches found
// then, for as long as it stays. A database that lands while a pad is
// attached applies at its next attach. Keyed by slot id, so a pad that moves
// from Standard to Direct is a new attach with the Direct path's observations.

#pragma once

#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/ControllerRecord.h"
#include "core/controllerdb/DirectModel.h"
#include "core/controllerdb/ExpectCheck.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dish::controllerdb {

struct PadAttach {
    std::string slotId;
    DeviceId id;
    bool bluetooth = false;
    // What the Direct path saw; absent for a pad another path carries, which checks nothing.
    std::optional<ObservedHardware> observed;
};

struct AttachedIdentity {
    std::optional<ControllerRecord> record;
    bool inert = false;
    std::vector<std::string> mismatches;

    bool operator==(const AttachedIdentity&) const = default;
};

using AttachedIdentities = std::map<std::string, AttachedIdentity>;

inline const ControllerRecord* recordForPad(const ControllerCatalog& catalog,
                                            const PadAttach& pad) {
    return pad.bluetooth ? catalog.findBluetooth(pad.id) : catalog.findUsb(pad.id);
}

inline AttachedIdentity identityAtAttach(const ControllerCatalog& catalog, const PadAttach& pad) {
    AttachedIdentity identity;
    const ControllerRecord* record = recordForPad(catalog, pad);
    if (record == nullptr) { return identity; }
    identity.record = *record;
    identity.inert = isInert(*record);
    if (pad.observed.has_value()) {
        identity.mismatches = expectMismatches(record->expect, *pad.observed);
    }
    return identity;
}

// Pads still here keep what they were given; new ones are read against the catalog in force;
// pads that left are forgotten, so a replug is read afresh.
inline AttachedIdentities reconcileAttached(const AttachedIdentities& previous,
                                            const std::vector<PadAttach>& present,
                                            const ControllerCatalog& catalog) {
    AttachedIdentities next;
    for (const PadAttach& pad : present) {
        const auto kept = previous.find(pad.slotId);
        next[pad.slotId] = kept != previous.end() ? kept->second : identityAtAttach(catalog, pad);
    }
    return next;
}

} // namespace dish::controllerdb
