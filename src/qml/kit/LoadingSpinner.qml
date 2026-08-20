// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Never shipped bare: the caption names what is being waited on.

// Bound so the ring's Repeater delegate can reference the outer `spinner` id
// and its own `required` index statically.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

ColumnLayout {
    id: spinner

    property string text: ""
    property bool running: true

    spacing: Tokens.s6

    BusyIndicator {
        running: spinner.running
        // Hidden when idle so the column collapses instead of leaving a gap.
        visible: spinner.running
        implicitWidth: Tokens.s11
        implicitHeight: Tokens.s11
        Layout.alignment: Qt.AlignHCenter

        // Replaced because Basic's default contentItem paints fixed greys that
        // clash with the card surface.
        contentItem: Item {
            implicitWidth: Tokens.s11
            implicitHeight: Tokens.s11

            Repeater {
                model: 8
                delegate: Rectangle {
                    id: dot
                    required property int index
                    readonly property real angle: dot.index / 8 * 2 * Math.PI
                    width: 4
                    height: 4
                    radius: width / 2
                    color: Theme.primary
                    // Trailing fade so the spin reads directional.
                    opacity: (dot.index + 1) / 8
                    x: 16 + Math.cos(dot.angle) * 12 - width / 2
                    y: 16 + Math.sin(dot.angle) * 12 - height / 2
                }
            }

            RotationAnimation on rotation {
                running: spinner.running && !Tokens.reducedMotion
                from: 0
                to: 360
                duration: Tokens.durBusy
                loops: Animation.Infinite
            }
        }
    }

    Label {
        text: spinner.text
        visible: spinner.text.length > 0
        color: Theme.mutedStrong
        font.pixelSize: Tokens.textSummary
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        Layout.alignment: Qt.AlignHCenter
        Layout.maximumWidth: 280
    }
}
