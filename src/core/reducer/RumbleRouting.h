// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The routing decision for the MSG_RUMBLE (0x0009) return path: what to actuate,
// not how. The receive thread snapshots the live connection-to-slot bindings once
// into an immutable view and decides through these functions, so it never
// re-reads live state mid-resolve. Actuation runs on the SDL thread via
// OutputCommandQueue.

#pragma once

#include <QString>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace dish::reducer {

// 16-bit wire magnitude down to 8 bits. Returns 0 only for exact zero: any tiny
// non-zero magnitude clamps up to 1, so an imperceptible buzz never becomes
// silent. The +32767 bias gives even rounding.
inline int rumbleMagnitudeTo255(int magnitude) {
    const int clamped = std::clamp(magnitude, 0, 65535);
    if (clamped == 0) { return 0; }
    const int scaled = (clamped * 255 + 32767) / 65535;
    return std::clamp(scaled, 1, 255);
}

// 0 is preserved as the stop sentinel. The 1500ms cap stops a buggy or hostile
// satellite stranding a multi-second buzz on the pad.
inline int rumbleSafeDurationMs(int durationMs) {
    if (durationMs == 0) { return 0; }
    return std::clamp(durationMs, 1, 1500);
}

inline bool isRumbleStop(int strongMagnitude, int weakMagnitude, int durationMs) {
    return durationMs == 0 || (strongMagnitude == 0 && weakMagnitude == 0);
}

// (actuatorIndex, amplitude)
using RumbleActuator = std::pair<int, int>;

// A single actuator folds to max(strong, weak), so a weak-only effect is still
// felt. Zero amplitudes are dropped rather than submitted as empty commands.
inline std::vector<RumbleActuator> combinedRumblePlan(int vibratorCount, int strongAmp,
                                                      int weakAmp) {
    std::vector<RumbleActuator> out;
    if (vibratorCount <= 0) { return out; }
    if (vibratorCount >= 2) {
        if (strongAmp > 0) { out.emplace_back(0, strongAmp); }
        if (weakAmp > 0) { out.emplace_back(1, weakAmp); }
        return out;
    }
    const int amp = std::max(strongAmp, weakAmp);
    if (amp > 0) { out.emplace_back(0, amp); }
    return out;
}

// ── Target resolution ────────────────────────────────────────────────────────

// A flat view of one connection, captured once per dispatch so resolveRumble
// stays pure. A slot id here IS the SDL bridge device id, so there is no separate
// slot lookup. `boundDeviceId` is empty when nothing is bound.
struct RumbleConnectionSnapshot {
    QString connId;
    bool connected = false;
    QString boundDeviceId;
};

struct RumbleTarget {
    QString deviceId; // empty means drive nothing

    bool valid() const { return !deviceId.isEmpty(); }
    bool operator==(const RumbleTarget& o) const { return deviceId == o.deviceId; }
};

// A connected match wins over a non-connected one with the same id, so a stale
// session cannot steal a live controller's rumble; among equally connected
// matches the first in snapshot order wins.
inline RumbleTarget resolveRumble(const std::vector<RumbleConnectionSnapshot>& connections,
                                  const QString& connId) {
    if (connId.isEmpty()) { return {}; }
    const RumbleConnectionSnapshot* chosen = nullptr;
    for (const auto& c : connections) {
        if (c.connId != connId) { continue; }
        if (c.connected) {
            chosen = &c;
            break;
        }
        if (chosen == nullptr) { chosen = &c; }
    }
    if (chosen == nullptr) { return {}; }
    if (chosen->boundDeviceId.isEmpty()) { return {}; }
    return RumbleTarget{chosen->boundDeviceId};
}

} // namespace dish::reducer
