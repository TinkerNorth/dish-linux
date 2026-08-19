// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// USB interrupt-endpoint poll-rate math, per the USB spec: a full-speed endpoint
// encodes bInterval as milliseconds, a high-speed one as a power-of-two exponent
// of 125us microframes. The descriptor fields come from WinUSB_QueryPipe /
// HidD_GetAttributes.

#pragma once

#include <cstdint>

namespace dish::reducer {

// A descriptor reporting a larger max packet size is high-speed.
inline constexpr int kMaxFsInterruptPacket = 64;

// The theoretical rate from bInterval. The exponent is clamped to [0,15] so an
// absurd bInterval cannot overflow the shift.
inline int computeUsbPollRateHz(int epInterval, int epMaxPacketSize) {
    if (epInterval <= 0) { return 0; }
    const bool isHighSpeed = epMaxPacketSize > kMaxFsInterruptPacket;
    std::int64_t periodMicros = 0;
    if (isHighSpeed) {
        int exp = epInterval - 1;
        if (exp < 0) { exp = 0; }
        if (exp > 15) { exp = 15; }
        periodMicros = (std::int64_t{1} << exp) * 125;
    } else {
        periodMicros = static_cast<std::int64_t>(epInterval) * 1000;
    }
    if (periodMicros <= 0) { return 0; }
    return static_cast<int>(1'000'000 / periodMicros);
}

// The observed rate over a sampling window. A counter reset shows up as a
// negative delta and reports 0 rather than a negative rate.
inline int measuredPollRateHz(std::int64_t deltaCount, std::int64_t deltaMs) {
    if (deltaMs <= 0) { return 0; }
    if (deltaCount <= 0) { return 0; }
    return static_cast<int>((deltaCount * 1000) / deltaMs);
}

} // namespace dish::reducer
