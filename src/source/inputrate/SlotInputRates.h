// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SlotInputRates — current and peak Hz per input stream for one slot, published
// by InputRateStore and read by the dashboard's live-stats display.
//
// Pure value type, Qt-free, ==-comparable so it can sit inside an Observable
// map whose distinct-until-changed suppresses no-op re-emits.

#pragma once

namespace dish::source {

struct SlotInputRates {
    // Gamepad report stream (buttons / sticks / triggers).
    int gamepadHz = 0;
    int gamepadPeakHz = 0;
    // Motion (gyro / accel) sample stream.
    int motionHz = 0;
    int motionPeakHz = 0;

    // Flowing now or ever peaked. Drives whether the live-stats row is worth
    // showing for the slot at all.
    bool hasAny() const {
        return gamepadHz > 0 || gamepadPeakHz > 0 || motionHz > 0 || motionPeakHz > 0;
    }

    bool operator==(const SlotInputRates& o) const {
        return gamepadHz == o.gamepadHz && gamepadPeakHz == o.gamepadPeakHz &&
               motionHz == o.motionHz && motionPeakHz == o.motionPeakHz;
    }
    bool operator!=(const SlotInputRates& o) const { return !(*this == o); }
};

} // namespace dish::source
