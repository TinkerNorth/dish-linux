// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The app's ONE tooltip. A bare `ToolTip` paints Basic's `palette.toolTipBase`
// / `toolTipText` — Qt's system defaults, which read as an unthemed slab in
// both appearances. DECLARE this, never attach it: the attached `ToolTip.text`
// resolves its delegate through `QtQuick.Controls`, which the pages do not
// import, so it logs "Component is not ready" and no tip appears.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

ToolTip {
    id: control

    // The desktop dwell. Call sites may shorten it, never lengthen it.
    delay: 500

    topPadding: Tokens.s3
    bottomPadding: Tokens.s3
    leftPadding: Tokens.s4
    rightPadding: Tokens.s4

    background: Rectangle {
        color: Theme.surface
        border.width: 1
        border.color: Theme.outline
        radius: Tokens.radiusButton
    }

    contentItem: Text {
        text: control.text
        color: Theme.onSurface
        font.pixelSize: Tokens.textSummary
        wrapMode: Text.WordWrap
    }
}
