// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which live-rate chip a slot card shows, and whether it reads as a live or a
// peak value. Kept Qt-free so the rules are testable without a widget; the card
// formats the localized "%1 Hz" / "~%1 Hz" string from the result.

#pragma once

#include "Models/Models.h"

namespace dish::ui {

enum class RateChipKind {
    Hidden, // no measurement worth showing (idle, never measured)
    Live,   // a current measurement: "<hz> Hz"
    Peak,   // only a high-water mark: "~<hz> Hz"
};

struct RateChip {
    RateChipKind kind = RateChipKind::Hidden;
    int hz = 0;
    bool operator==(const RateChip& o) const { return kind == o.kind && hz == o.hz; }
    bool operator!=(const RateChip& o) const { return !(*this == o); }
};

// A USB-direct pad streams continuously, so its live Hz is the measurement. Any
// other pad only reports while actuated, so the peak window approximates it.
inline RateChip gamepadRateChip(const models::SlotLiveRates& r, bool usbDirect) {
    if (usbDirect && r.gamepadHz > 0) { return RateChip{RateChipKind::Live, r.gamepadHz}; }
    if (r.gamepadPeakHz > 0) { return RateChip{RateChipKind::Peak, r.gamepadPeakHz}; }
    return RateChip{RateChipKind::Hidden, 0};
}

// Motion is genuinely event-driven from the IMU, so there is no peak arm. The
// caller gates on hasMotion.
inline RateChip motionRateChip(const models::SlotLiveRates& r) {
    if (r.motionHz > 0) { return RateChip{RateChipKind::Live, r.motionHz}; }
    return RateChip{RateChipKind::Hidden, 0};
}

// The URB completion rate: always live, since the poll loop runs continuously
// while the pad is claimed.
inline RateChip pollRateChip(const models::SlotLiveRates& r, bool usbDirect) {
    if (usbDirect && r.directPollHz > 0) { return RateChip{RateChipKind::Live, r.directPollHz}; }
    return RateChip{RateChipKind::Hidden, 0};
}

} // namespace dish::ui
