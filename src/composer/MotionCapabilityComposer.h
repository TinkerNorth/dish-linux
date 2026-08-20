// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Derives, per slot, the motion capability the dish advertises and displays.
// The desktop clients are physical-controllers-only, so the map is keyed on
// device slot ids.

#pragma once

#include "architecture/Composer.h"
#include "architecture/Observable.h"
#include "core/model/Protocol.h"
#include "source/store/SatelliteMotionBackendStatusStore.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dish::composer {

// ── Upstream snapshot shapes (what the transform reads) ──────────────────────

struct MotionDevice {
    std::string slotId;
    bool hasGyro = false;

    bool operator==(const MotionDevice& o) const {
        return slotId == o.slotId && hasGyro == o.hasGyro;
    }
};

struct MotionConnection {
    std::string id;
    bool isSatellite = true;
    bool connected = false; // live satellite session
    // slotId -> applied controller type. An absent slot means "type unknown",
    // which is treated as sink-supported so no false warning is raised.
    std::map<std::string, int> slotControllerTypes;

    bool operator==(const MotionConnection& o) const {
        return id == o.id && isSatellite == o.isSatellite && connected == o.connected &&
               slotControllerTypes == o.slotControllerTypes;
    }
};

using MotionDeviceList = std::vector<MotionDevice>;
using MotionConnectionList = std::vector<MotionConnection>;
// slotId -> bound connection id.
using MotionBindings = std::map<std::string, std::string>;
// slotId -> user motion-enable toggle (absent = default-on).
using MotionEnabledMap = std::map<std::string, bool>;

// ── Derived per-slot capability ──────────────────────────────────────────────

struct MotionCapability {
    bool hasGyro = false;
    bool carriesOnConnection = false;
    bool userEnabled = true;
    bool hostHasSinkForType = true;
    std::optional<source::SatelliteMotionBackendStatus> satelliteBackendStatus;

    // Whether to read the gyro at all. NOT gated on the satellite backend
    // status: that governs the user-facing indicator, not our listener.
    bool effective() const { return hasGyro && carriesOnConnection && userEnabled; }

    // CAP_MOTION iff the dish CAN emit motion. Deliberately NOT gated on
    // carriesOnConnection (a reconnect must recover motion without a fresh
    // handshake) nor on receiver health (that is the receiver's concern).
    int toCapBits() const {
        return (hasGyro && userEnabled) ? static_cast<int>(proto::kCapMotion) : 0;
    }

    static MotionCapability off() {
        return MotionCapability{false, false, true, true, std::nullopt};
    }

    bool operator==(const MotionCapability& o) const {
        return hasGyro == o.hasGyro && carriesOnConnection == o.carriesOnConnection &&
               userEnabled == o.userEnabled && hostHasSinkForType == o.hostHasSinkForType &&
               satelliteBackendStatus == o.satelliteBackendStatus;
    }
    bool operator!=(const MotionCapability& o) const { return !(*this == o); }
};

// Duplicates MotionEnabledStore::kDefaultEnabled so the pure transform needs no
// dependency on the store.
inline constexpr bool kDefaultMotionEnabled = true;

using MotionCapabilityMap = std::map<std::string, MotionCapability>;

// A device that is present but unbound still gets an entry, with
// carriesOnConnection=false. Pure.
MotionCapabilityMap
deriveMotionCapabilities(const MotionDeviceList& devices, const MotionBindings& bindings,
                         const MotionConnectionList& connections, const MotionEnabledMap& enabled,
                         const source::SatelliteMotionBackendStatusMap& backend);

class MotionCapabilityComposer
    : public arch::Composer<MotionCapabilityMap, MotionDeviceList, MotionBindings,
                            MotionConnectionList, MotionEnabledMap,
                            source::SatelliteMotionBackendStatusMap> {
  public:
    MotionCapabilityComposer(
        const arch::Observable<MotionDeviceList>& devices,
        const arch::Observable<MotionBindings>& bindings,
        const arch::Observable<MotionConnectionList>& connections,
        const arch::Observable<MotionEnabledMap>& enabled,
        const arch::Observable<source::SatelliteMotionBackendStatusMap>& backend)
        : arch::Composer<MotionCapabilityMap, MotionDeviceList, MotionBindings,
                         MotionConnectionList, MotionEnabledMap,
                         source::SatelliteMotionBackendStatusMap>(
              devices, bindings, connections, enabled, backend, deriveMotionCapabilities) {}

    // Off() for an unknown slot.
    MotionCapability capabilityFor(const std::string& slotId) const {
        const auto& snapshot = state().value();
        const auto it = snapshot.find(slotId);
        if (it == snapshot.end()) { return MotionCapability::off(); }
        return it->second;
    }
};

} // namespace dish::composer
