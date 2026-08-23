// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A ColumnLayout, NOT a Column: this row goes inside a GridLayout cell, and a
// positioner skips any child whose width is 0, which latches the whole
// component at 0x0 with every child unpositioned at y == 0.
//
// Two signals, two consumers: push `moved` into the live processor so the pad
// responds under the user's thumb, and persist only on `committed` — a drag
// emits a value per pixel, each of which would otherwise be a repository write.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

ColumnLayout {
    id: control

    property string label: ""
    property int value: 0
    property int minValue: 0
    property int maxValue: 30
    // The readout follows the HANDLE, so a caller that wants its own wording
    // formats `displayValue` rather than its own `value`.
    readonly property int displayValue: Math.round(slider.value)
    property string valueText: control.displayValue + "%"

    // Live, per-step. Never persist from here.
    signal moved(int value)
    // Terminal, on release. Persist from here.
    signal committed(int value)

    spacing: Tokens.s4

    RowLayout {
        Layout.fillWidth: true
        spacing: Tokens.s4

        Text {
            id: nameText
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignBaseline
            text: control.label
            font.pixelSize: Tokens.textSummary
            color: control.enabled ? Theme.onSurface : Theme.disabledFg
            elide: Text.ElideRight
        }
        Text {
            id: pctText
            Layout.alignment: Qt.AlignBaseline
            // The slider's own value, not `control.value`: a drag breaks the
            // inbound binding, and the readout must follow the handle.
            text: control.valueText
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textMeta
            color: Theme.muted
        }
    }

    Slider {
        id: slider
        Layout.fillWidth: true
        from: control.minValue
        to: control.maxValue
        stepSize: 1
        value: control.value
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        opacity: slider.enabled ? 1.0 : Tokens.disabledOpacity

        // No horizontal padding: the track takes the cell. The vertical padding
        // is the knob's growth room.
        padding: 0
        topPadding: Tokens.s1
        bottomPadding: Tokens.s1

        Accessible.role: Accessible.Slider
        Accessible.name: control.label

        onMoved: control.moved(Math.round(slider.value))
        onPressedChanged: if (!slider.pressed) control.committed(Math.round(slider.value))

        // Qt's Slider handles the arrow / page / home / end keys itself and
        // emits moved(), but has no notion of a gesture ending, so the commit
        // hangs off the key release.
        Keys.onReleased: event => {
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                    || event.key === Qt.Key_Up || event.key === Qt.Key_Down
                    || event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown
                    || event.key === Qt.Key_Home || event.key === Qt.Key_End)
                control.committed(Math.round(slider.value));
        }

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            // A replaced background contributes nothing to the Control's
            // implicit size, so it must carry its own. No implicit WIDTH: the
            // track has no natural measure, it takes the cell.
            implicitHeight: 4
            width: slider.availableWidth
            height: 4
            radius: Tokens.radiusBar
            color: Theme.surfaceDim
            border.width: 1
            border.color: slider.enabled ? Theme.outline : Theme.disabledFg

            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: Tokens.radiusBar
                color: slider.enabled ? Theme.primary : Theme.disabledFg
            }
        }

        handle: Item {
            id: grip
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - grip.width)
            y: slider.topPadding + slider.availableHeight / 2 - grip.height / 2
            implicitWidth: 12
            implicitHeight: 12
            width: (slider.pressed || slider.hovered || slider.visualFocus) ? 14 : 12
            height: grip.width

            Behavior on width {
                enabled: !Tokens.reducedMotion
                NumberAnimation { duration: Tokens.durFast }
            }

            Rectangle {
                anchors.fill: parent
                radius: grip.width / 2
                color: slider.enabled ? Theme.primary : Theme.disabledFg
            }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                radius: (grip.width + 4) / 2
                visible: slider.visualFocus
                color: "transparent"
                border.width: 2
                border.color: Theme.focusRing
            }
        }
    }
}
