// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/SatelliteMotionBackendStatusStore.h"

namespace dish::source {

std::map<std::string, SatelliteMotionBackendStatus>
SatelliteMotionBackendStatusStore::slotStatusesFor(
    const std::string& connectionId, const std::vector<std::string>& boundSlotIds) const {
    const auto& snapshot = state().value();
    std::map<std::string, SatelliteMotionBackendStatus> out;
    for (const auto& slotId : boundSlotIds) {
        const auto it = snapshot.find({connectionId, slotId});
        if (it == snapshot.end()) { continue; }
        out[slotId] = it->second;
    }
    return out;
}

} // namespace dish::source
