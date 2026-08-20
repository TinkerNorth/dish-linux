// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The text-only action: everything secondary in the installer's link rows.
// Accent because it acts; `quiet` exists for a link that de-emphasises next
// to a sibling (never for a disabled one — that is `enabled: false`).

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

AbstractButton {
    id: control

    property bool quiet: false

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    // The visible glyphs are 12px; the padding buys back a clickable target
    // without inflating the row visually (the surface is transparent).
    topPadding: Tokens.s2
    bottomPadding: Tokens.s2
    leftPadding: Tokens.s1
    rightPadding: Tokens.s1

    font.pixelSize: Tokens.textSummary

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.Button
    Accessible.name: control.text

    // Enter activates a focused link, same convention DishButton wires.
    Keys.onReturnPressed: control.clicked()
    Keys.onEnterPressed: control.clicked()

    HoverHandler { cursorShape: Qt.PointingHandCursor }

    contentItem: Text {
        text: control.text
        // Piecewise, not `font: control.font`: the grouped binding and the
        // underline sub-property below would assign the group twice.
        font.pixelSize: control.font.pixelSize
        font.weight: control.font.weight
        // Hover states with underline, not colour alone.
        font.underline: control.enabled && (control.hovered || control.visualFocus)
        color: !control.enabled ? Theme.disabledFg
             : control.down ? Theme.primaryDark
             : control.quiet ? Theme.muted
             : Theme.primary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: "transparent"
        radius: Tokens.radiusChip
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focusRing
    }
}
