// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A plain checkbox row. LabeledSwitch stays the app's on/off idiom; this is
// the installer's opt-in mark (shortcuts, purge) where a switch would read as
// a live setting rather than a choice being collected.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

AbstractButton {
    id: control

    checkable: true

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    spacing: Tokens.s4
    padding: Tokens.s1

    font.pixelSize: Tokens.textBase

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.CheckBox
    Accessible.name: control.text
    Accessible.checked: control.checked

    HoverHandler { cursorShape: Qt.PointingHandCursor }

    indicator: Rectangle {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 14
        height: 14
        radius: 4
        color: control.checked ? Theme.primary : "transparent"
        border.width: 1
        border.color: !control.enabled ? Theme.disabledFg
                    : control.checked ? Theme.primary
                    : control.hovered ? Theme.primary
                    : Theme.outline

        Behavior on color {
            enabled: !Tokens.reducedMotion
            ColorAnimation { duration: Tokens.durFast }
        }

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            font.pixelSize: Tokens.textChip
            color: Theme.onPrimary
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: 6
            visible: control.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.enabled ? Theme.onSurface : Theme.disabledFg
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
    }
}
