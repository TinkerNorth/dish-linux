// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// KitTextField's multi-line sibling, for prose a person writes in their own
// words (the support report). The same frame, focus ring and palette; the text
// wraps, and the field starts at three lines and grows with what is written.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

TextArea {
    id: area

    // TextArea is a TextEdit, not a Control, so it has no `visualFocus`; the same rule as
    // KitTextField keeps a click into the field from drawing the ring.
    readonly property bool keyboardFocus: area.activeFocus
                                          && (area.focusReason === Qt.TabFocusReason
                                              || area.focusReason === Qt.BacktabFocusReason
                                              || area.focusReason === Qt.ShortcutFocusReason)

    implicitHeight: Math.max(88, area.contentHeight + area.topPadding + area.bottomPadding)
    topPadding: Tokens.s3
    bottomPadding: Tokens.s3
    leftPadding: Tokens.s4
    rightPadding: Tokens.s4
    wrapMode: TextEdit.Wrap
    color: Theme.onSurface
    placeholderTextColor: Theme.muted
    selectionColor: Theme.primary
    selectedTextColor: Theme.onPrimary
    font.pixelSize: Tokens.textBase

    Accessible.name: area.placeholderText

    background: Item {
        Rectangle {
            id: frame
            anchors.fill: parent
            radius: Tokens.radiusButton
            color: Theme.surface
            border.width: 1
            border.color: area.activeFocus ? Theme.primary : Theme.outline

            Behavior on border.color {
                enabled: !Tokens.reducedMotion
                ColorAnimation { duration: Tokens.durFast }
            }
        }

        Rectangle {
            anchors.fill: frame
            anchors.margins: -2
            radius: Tokens.radiusButton + 2
            visible: area.keyboardFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }
}
