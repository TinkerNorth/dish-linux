// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The drop-down trigger. It emits `picked` and never holds its own truth — the
// caller streams the selection back through `value`. A real AbstractButton, not
// a Rectangle with a MouseArea, so it is keyboard-reachable and Narrator calls
// it a combo box.

// Bound: the Repeater delegate references the outer `control` id alongside its
// `required` model props — static resolution needs bound component behavior.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

AbstractButton {
    id: control

    property var options: []
    property string value: ""
    // Hosts that own window-level Esc need to stand down while the menu has
    // the keyboard.
    readonly property alias menuOpen: menu.visible

    signal picked(string option)

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    topPadding: Tokens.s3
    bottomPadding: Tokens.s3
    leftPadding: Tokens.s6
    rightPadding: Tokens.s6

    implicitHeight: Math.max(Tokens.minTouch,
                             control.implicitContentHeight
                             + control.topPadding + control.bottomPadding)

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.ComboBox
    Accessible.name: control.value

    onClicked: menu.open()
    Keys.onDownPressed: menu.open()

    HoverHandler { cursorShape: Qt.PointingHandCursor }

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: Tokens.radiusButton
            // A filled recess, so the interaction wash tints into it rather
            // than laying over a transparent ground.
            color: control.down ? Qt.tint(Theme.surfaceDim, Theme.primaryPress)
                 : control.hovered ? Qt.tint(Theme.surfaceDim, Theme.primaryHover)
                 : Theme.surfaceDim
            border.width: 1
            border.color: control.enabled ? Theme.outline : Theme.disabledFg

            Behavior on color {
                enabled: !Tokens.reducedMotion
                ColorAnimation { duration: Tokens.durFast }
            }
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: Tokens.radiusButton + 2
            visible: control.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }

    contentItem: Row {
        spacing: Tokens.s5

        Text {
            text: control.value
            font.pixelSize: Tokens.textSummary
            color: control.enabled ? Theme.onSurface : Theme.disabledFg
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: "▾"
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textChip
            color: control.enabled ? Theme.muted : Theme.disabledFg
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Menu {
        id: menu
        y: control.height + Tokens.s1

        background: Rectangle {
            implicitWidth: Math.max(control.width, 140)
            color: Theme.surface
            border.width: 1
            border.color: Theme.outline
            radius: Tokens.radiusButton
        }

        Repeater {
            model: control.options
            delegate: MenuItem {
                id: item
                required property string modelData
                text: item.modelData

                Accessible.role: Accessible.MenuItem
                Accessible.name: item.modelData

                contentItem: Text {
                    text: item.modelData
                    font.pixelSize: Tokens.textSummary
                    color: item.modelData === control.value ? Theme.primary : Theme.onSurface
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: item.highlighted ? Theme.primaryHover : "transparent"
                    radius: Tokens.radiusChip
                }
                onTriggered: control.picked(item.modelData)
            }
        }
    }
}
