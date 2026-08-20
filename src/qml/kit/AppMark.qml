// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The app identity mark (satellite dish in a gear) — the window icon's
// identity as a glyph: 16px in the title bar, and the same asset marks Home
// in the rail. `busy` runs the one sanctioned brand transient (WireLine's
// opacity pulse) to say work is in flight without composing a scene.

import QtQuick
import Dish.Chrome

BrandGlyph {
    id: mark

    property bool busy: false

    glyph: "dish-logo"

    SequentialAnimation on opacity {
        running: mark.busy && mark.visible && !Tokens.reducedMotion
        loops: Animation.Infinite
        // The pulse always lands back at rest, so stopping mid-cycle (work
        // finished) never strands a dimmed mark.
        alwaysRunToEnd: true
        NumberAnimation { to: 0.45; duration: Tokens.durBusy / 2 }
        NumberAnimation { to: 1.0; duration: Tokens.durBusy / 2 }
    }
}
