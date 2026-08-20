// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// This component OWNS rate/latency formatting; nothing else in the app formats
// a rate. `~` means "derived or estimated". Colour carries live-vs-idle only.

import QtQuick
import Dish.Chrome

Text {
    // Whether the reported thing is live, NOT whether the number was measured
    // (that is the `~`).
    property bool live: false

    font.family: Tokens.monoFamily
    font.pixelSize: Tokens.textChip
    color: live ? Theme.success : Theme.muted

    function rateText(hz, measured) {
        if (!hz || hz <= 0)
            return "";
        return (measured ? "" : "~") + Math.round(hz) + " Hz";
    }

    // Sub-millisecond reads "<1 ms": the figure is real but under this
    // instrument's resolution, and "~0.0 ms" would claim a latency no link has.
    // Mirrors reducer::formatLatencyMs, which is what the models render — the
    // two must never disagree.
    function latencyText(ms, samples) {
        if (!samples || samples <= 0)
            return "";
        const tenths = Math.round(Math.max(0, Number(ms)) * 10);
        if (tenths < 10)
            return "<1 ms";
        return "~" + (tenths / 10).toFixed(1) + " ms";
    }
}
