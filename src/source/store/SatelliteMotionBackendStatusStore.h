// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SatelliteMotionBackendStatusStore — a StateSource over the per-(connection,
// slot) motion backend status the satellite reports in its enriched heartbeat /
// session ack: whether the host has a motion SINK for the slot's controller type
// (`sinkSupportedForType`) and whether the motion backend is healthy
// (`backendOk`). Port of dish-android source/store/SatelliteMotionBackendStatusStore
// (the SatelliteMotionBackendStatus value type + the AbstractStateSource over a
// Map<(conn,slot), status>).
//
// SatelliteMotionBackendStatus::fromFlags decodes a 0..255 protocol-1 flag byte:
// FLAG_SINK_SUPPORTED_FOR_TYPE=0x01, FLAG_BACKEND_OK=0x02; reserved upper bits
// are ignored. `effective` = both bits set. The caller short-circuits the
// sentinel "no extended ack" before reaching fromFlags, so this only handles a
// real flag byte.

#pragma once

#include "architecture/StateSource.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dish::source {

// Decoded per-slot motion backend status (one observation from the satellite).
struct SatelliteMotionBackendStatus {
    bool sinkSupportedForType = false;
    bool backendOk = false;

    // FLAG_* match satellite/dish-android: the low two bits of the ack flag byte.
    static constexpr int kFlagSinkSupportedForType = 0x01;
    static constexpr int kFlagBackendOk = 0x02;

    // Both conditions hold => motion actually lands on the host.
    bool effective() const { return sinkSupportedForType && backendOk; }

    // Decode a 0..255 flag byte. Reserved upper bits are ignored.
    static SatelliteMotionBackendStatus fromFlags(int flags) {
        return SatelliteMotionBackendStatus{(flags & kFlagSinkSupportedForType) != 0,
                                            (flags & kFlagBackendOk) != 0};
    }

    bool operator==(const SatelliteMotionBackendStatus& o) const {
        return sinkSupportedForType == o.sinkSupportedForType && backendOk == o.backendOk;
    }
    bool operator!=(const SatelliteMotionBackendStatus& o) const { return !(*this == o); }
};

// (connectionId, slotId) -> status. std::map gives a deterministic,
// ==-comparable value so the Observable's distinct-until-changed suppresses
// no-op re-emits (a clear() of an absent key emits nothing).
using SatelliteMotionBackendStatusMap =
    std::map<std::pair<std::string, std::string>, SatelliteMotionBackendStatus>;

class SatelliteMotionBackendStatusStore
    : public arch::StateSource<SatelliteMotionBackendStatusMap> {
  public:
    SatelliteMotionBackendStatusStore()
        : arch::StateSource<SatelliteMotionBackendStatusMap>(SatelliteMotionBackendStatusMap{}) {}

    // The status for a (connection, slot), or nullopt when none observed.
    std::optional<SatelliteMotionBackendStatus> statusFor(const std::string& connectionId,
                                                          const std::string& slotId) const {
        const auto& snapshot = state().value();
        const auto it = snapshot.find({connectionId, slotId});
        if (it == snapshot.end()) { return std::nullopt; }
        return it->second;
    }

    // Record (overwrite) the status for a (connection, slot).
    void setStatus(const std::string& connectionId, const std::string& slotId,
                   const SatelliteMotionBackendStatus& status) {
        setState([&](const SatelliteMotionBackendStatusMap& current) {
            SatelliteMotionBackendStatusMap next = current;
            next[{connectionId, slotId}] = status;
            return next;
        });
    }

    // Drop exactly one (connection, slot). A no-op (no emit) if it was absent;
    // sibling slots survive.
    void clear(const std::string& connectionId, const std::string& slotId) {
        setState([&](const SatelliteMotionBackendStatusMap& current) {
            const std::pair<std::string, std::string> key{connectionId, slotId};
            if (current.find(key) == current.end()) { return current; }
            SatelliteMotionBackendStatusMap next = current;
            next.erase(key);
            return next;
        });
    }

    // Drop every slot under a connection at once, leaving other connections
    // untouched (e.g. when a connection is forgotten).
    void clearConnection(const std::string& connectionId) {
        setState([&](const SatelliteMotionBackendStatusMap& current) {
            SatelliteMotionBackendStatusMap next;
            for (const auto& [key, status] : current) {
                if (key.first != connectionId) { next.emplace(key, status); }
            }
            return next;
        });
    }

    // The subset of `boundSlotIds` under `connectionId` that carry a status, as
    // slotId -> status. Slots with no observation are omitted. Non-inline (.cpp).
    std::map<std::string, SatelliteMotionBackendStatus>
    slotStatusesFor(const std::string& connectionId,
                    const std::vector<std::string>& boundSlotIds) const;
};

} // namespace dish::source
