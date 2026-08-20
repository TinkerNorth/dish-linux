// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Emits picked(option); the caller applies the change and streams the selection
// back through `value`, so the control never holds its own truth. ←/→ MOVE the
// selection: a control with one value has no gap between focused and chosen.
//
// Disabled drops the accent thumb for a neutral surface + hairline. The whole
// control is at disabledOpacity there, where `onPrimary` (tuned to read on a
// saturated fill) reads as neither, and a dead control still has to report
// which segment is selected.

// Bound: the Repeater delegate references the outer `control` id alongside its
// `required` model props.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

Control {
    id: control

    property var options: []
    property string value: ""
    property bool small: false
    // Dimmed + inert while an async apply is in flight (claiming a pad).
    property bool busy: false

    signal picked(string option)

    readonly property int framePad: control.small ? 2 : 3
    readonly property bool interactive: control.enabled && !control.busy

    focusPolicy: Qt.StrongFocus
    padding: control.framePad
    opacity: control.interactive ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.Grouping
    Accessible.name: control.value

    Keys.onLeftPressed: control.stepSelection(-1)
    Keys.onRightPressed: control.stepSelection(1)

    // Clamped, never wrapping: a wrap would let a held arrow key cycle a
    // destructive path choice.
    function stepSelection(delta) {
        if (!control.interactive)
            return;
        const i = control.options.indexOf(control.value);
        if (i < 0)
            return;
        const next = i + delta;
        if (next >= 0 && next < control.options.length)
            control.picked(control.options[next]);
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: control.small ? Tokens.radiusButton : Tokens.radiusCard
            color: Theme.surfaceDim
            border.width: 1
            border.color: control.enabled ? Theme.outline : Theme.disabledFg
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: (control.small ? Tokens.radiusButton : Tokens.radiusCard) + 2
            visible: control.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }

    contentItem: Row {
        spacing: control.small ? 3 : 4

        Repeater {
            model: control.options
            delegate: Rectangle {
                id: segment
                required property string modelData
                readonly property bool selected: segment.modelData === control.value

                width: label.implicitWidth + (control.small ? 18 : 32)
                height: label.implicitHeight + (control.small ? 5 : 10)
                radius: Tokens.radiusButton - control.framePad
                color: !segment.selected ? "transparent"
                     : control.enabled ? Theme.primary
                     : Theme.surface
                border.width: segment.selected && !control.enabled ? 1 : 0
                border.color: Theme.outline

                Accessible.role: Accessible.RadioButton
                Accessible.name: segment.modelData
                Accessible.checked: segment.selected

                Behavior on color {
                    enabled: !Tokens.reducedMotion
                    ColorAnimation { duration: Tokens.durFast }
                }

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: segment.modelData
                    font.pixelSize: control.small ? Tokens.textChip : Tokens.textBase
                    font.weight: segment.selected ? Font.DemiBold : Font.Normal
                    color: !control.enabled ? Theme.disabledFg
                         : segment.selected ? Theme.onPrimary
                         : hover.hovered ? Theme.onSurface
                         : Theme.muted
                }

                HoverHandler {
                    id: hover
                    enabled: control.interactive
                    cursorShape: Qt.PointingHandCursor
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: control.interactive
                    onClicked: control.picked(segment.modelData)
                }
            }
        }
    }
}
