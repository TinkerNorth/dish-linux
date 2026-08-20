// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The full-width list row; the whole row is the click target. A real
// AbstractButton, not a Rectangle with a MouseArea, so a keyboard user can
// reach it and Narrator announces it.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

AbstractButton {
    id: control

    property string title: ""
    property string subtitle: ""

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    topPadding: Tokens.s5
    bottomPadding: Tokens.s5
    leftPadding: Tokens.s6
    rightPadding: Tokens.s6

    implicitHeight: Math.max(Tokens.hitRow,
                             control.implicitContentHeight
                             + control.topPadding + control.bottomPadding)

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.Button
    Accessible.name: control.title
    Accessible.description: control.subtitle

    HoverHandler { cursorShape: Qt.PointingHandCursor }

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: Tokens.radiusCard
            // A filled surface takes its wash as a tint, not an overlay.
            color: control.down ? Qt.tint(Theme.surface, Theme.primaryPress)
                 : control.hovered ? Qt.tint(Theme.surface, Theme.primaryHover)
                 : Theme.surface
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
            radius: Tokens.radiusCard + 2
            visible: control.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }

    contentItem: RowLayout {
        spacing: Tokens.s5

        ColumnLayout {
            spacing: Tokens.s1
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            Text {
                text: control.title
                font.pixelSize: Tokens.textBase
                font.weight: Font.DemiBold
                color: control.enabled ? Theme.onSurface : Theme.disabledFg
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Text {
                text: control.subtitle
                visible: control.subtitle.length > 0
                font.pixelSize: Tokens.textMeta
                color: Theme.muted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Text {
            text: "›"
            font.pixelSize: Tokens.textHeading
            color: control.enabled ? Theme.muted : Theme.disabledFg
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
