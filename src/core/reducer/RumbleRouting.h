// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The routing decision for the MSG_RUMBLE (0x0009) return path: what to actuate,
// not how. Actuation runs on the SDL thread via OutputCommandQueue.

#pragma once

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

// ── The user's switch ────────────────────────────────────────────────────────

// One command for a pad's two motors: the levels, and how long they hold (0
// holds until the next command).
struct RumbleCommand {
    std::uint16_t strong = 0;
    std::uint16_t weak = 0;
    std::uint16_t durationMs = 0;

    bool operator==(const RumbleCommand&) const = default;
};

inline constexpr RumbleCommand kRumbleStop{};

// A slot the user switched off turns the host's command into a stop rather than
// dropping it: a Moonlight hold and a Direct claim's levels never expire on their
// own, so a motor running when the switch went off would run until it came back.
inline RumbleCommand rumbleTheUserAllows(const RumbleCommand& fromHost, bool userRumbleOn) {
    if (userRumbleOn) { return fromHost; }
    return kRumbleStop;
}

} // namespace dish::reducer
