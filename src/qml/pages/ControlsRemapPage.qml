// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Configure controls — the per-(vid,pid) raw-joystick remap for one slot. Each
// assign row arms a capture: click it, then physically press the input. Capture
// is an ARMED MODE, so it has escapes (Esc, a timeout, and three stopInputCapture
// exits) — a forgotten arm leaves the bridge streaming raw reports forever.

// Bound: the shared assign-row delegate resolves the page id and its required
// model bindings statically.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: page

    // Set by the pusher (ControllersPage) before this page becomes visible.
    property string slotId: ""
    property string slotName: ""

    readonly property string headerTitle: qsTr("Configure controls")
    readonly property string headerSub: page.slotName.length > 0
        ? qsTr("%1 · raw DirectInput remap").arg(page.slotName)
        : qsTr("Raw DirectInput remap")

    readonly property int bodyWidth: 760

    // How long an armed row waits for a deliberate input before giving up. Long
    // enough to reach across a desk, short enough that a mis-click self-heals.
    readonly property int captureTimeoutMs: 10000

    // The capture kinds the contract defines for assignSlotInput.
    readonly property int kindAxis: 0
    readonly property int kindButton: 1
    readonly property int kindHat: 2

    // Re-pulled on load and after every assign/invert/reset: slotRemap is
    // one-shot. An empty object means the slot resolved to no (vid,pid) — the
    // not-remappable note.
    property var remap: ({})

    // Only one row captures at a time.
    property string capturingTarget: ""
    property string capturingLabel: ""

    // Raised when a capture timed out, cleared the moment anything is armed
    // again — the note explains the row that just reverted, not a live state.
    property bool captureExpired: false

    readonly property bool capturing: page.capturingTarget.length > 0
    readonly property bool hasRemap: page.remap && page.remap.a !== undefined

    // The window losing focus is a leave: the bridge must not keep streaming raw
    // reports behind another app.
    readonly property bool windowActive: page.Window.active

    Component.onCompleted: page.refresh()

    // Three exits, not one. Component.onDestruction covers Back and a rail
    // switch that replaces the stack; onDeactivating covers a push on top of
    // this page; windowActive covers Alt-Tab and a minimise.
    Component.onDestruction: App.stopInputCapture()
    StackView.onDeactivating: page.cancelCapture()
    onWindowActiveChanged: if (!page.windowActive) page.cancelCapture()

    // Esc is the universal "get me out of this mode"; without it the
    // Stop capture button is the only exit and it is 400px from the pointer.
    Shortcut {
        sequence: "Esc"
        enabled: page.capturing
        onActivated: page.cancelCapture()
    }

    Timer {
        id: captureTimeout
        interval: page.captureTimeoutMs
        repeat: false
        onTriggered: {
            page.cancelCapture();
            page.captureExpired = true;
        }
    }

    function refresh() {
        page.remap = App.slotRemap(page.slotId);
    }

    // Re-pointing the filter from another row restarts the budget with it.
    function beginCapture(target, label) {
        page.captureExpired = false;
        page.capturingTarget = target;
        page.capturingLabel = label;
        App.startInputCapture(page.slotId);
        captureTimeout.restart();
    }

    function cancelCapture() {
        captureTimeout.stop();
        if (page.capturingTarget.length === 0) {
            return;
        }
        page.capturingTarget = "";
        page.capturingLabel = "";
        App.stopInputCapture();
    }

    // The contract's "unassigned" write. Clearing is not rebinding: it is the
    // only way back to an unbound output.
    function clearAssignment(target, kind) {
        page.cancelCapture();
        App.assignSlotInput(page.slotId, target, kind, -1);
        page.refresh();
    }

    // ---- Readouts -----------------------------------------------------------

    function isAssigned(value) {
        return value !== undefined && value !== null && value >= 0;
    }

    function indexLabel(value, kindWord) {
        if (!page.isAssigned(value)) {
            return qsTr("Unassigned");
        }
        return qsTr("%1 %2").arg(kindWord).arg(value);
    }

    // A trigger source is {kind:"axis"|"button", index:int}.
    function triggerLabel(obj) {
        if (!obj || !page.isAssigned(obj.index)) {
            return qsTr("Unassigned");
        }
        return obj.kind === "button" ? qsTr("Button %1 · digital").arg(obj.index)
                                     : qsTr("Axis %1 · analog").arg(obj.index);
    }

    // The d-pad readout: a per-direction button override wins; else the shared
    // hat; else unassigned.
    function dpadLabel(dirValue) {
        if (page.isAssigned(dirValue)) {
            return qsTr("Button %1").arg(dirValue);
        }
        if (page.isAssigned(page.remap.hatIndex)) {
            return qsTr("Hat %1").arg(page.remap.hatIndex);
        }
        return qsTr("Unassigned");
    }

    // ---- Row descriptors ----------------------------------------------------
    // { label, target, kind, value, assigned } — `kind` is what a Clear writes
    // alongside index -1, and withAssignment routes on it.

    function buttonRow(label, target, value) {
        return {
            "label": label,
            "target": target,
            "kind": page.kindButton,
            "value": page.indexLabel(value, qsTr("Button")),
            "assigned": page.isAssigned(value)
        };
    }

    // A direction with its own button clears as a Button; a direction reading
    // the shared hat clears as a Hat — which drops the hat for EVERY direction,
    // because one hat is one source.
    function dpadRow(label, target, value) {
        return {
            "label": label,
            "target": target,
            "kind": page.isAssigned(value) ? page.kindButton : page.kindHat,
            "value": page.dpadLabel(value),
            "assigned": page.isAssigned(value) || page.isAssigned(page.remap.hatIndex)
        };
    }

    function axisRow(label, target, value) {
        return {
            "label": label,
            "target": target,
            "kind": page.kindAxis,
            "value": page.indexLabel(value, qsTr("Axis")),
            "assigned": page.isAssigned(value)
        };
    }

    function triggerRow(label, target, obj) {
        return {
            "label": label,
            "target": target,
            "kind": obj && obj.kind === "button" ? page.kindButton : page.kindAxis,
            "value": page.triggerLabel(obj),
            "assigned": obj !== undefined && obj !== null && page.isAssigned(obj.index)
        };
    }

    readonly property var buttonRows: [
        page.buttonRow(qsTr("A (bottom)"), "a", page.remap.a),
        page.buttonRow(qsTr("B (right)"), "b", page.remap.b),
        page.buttonRow(qsTr("X (left)"), "x", page.remap.x),
        page.buttonRow(qsTr("Y (top)"), "y", page.remap.y),
        page.buttonRow(qsTr("Left shoulder"), "leftShoulder", page.remap.leftShoulder),
        page.buttonRow(qsTr("Right shoulder"), "rightShoulder", page.remap.rightShoulder),
        page.buttonRow(qsTr("Back"), "back", page.remap.back),
        page.buttonRow(qsTr("Start"), "start", page.remap.start),
        page.buttonRow(qsTr("Left thumb"), "leftThumb", page.remap.leftThumb),
        page.buttonRow(qsTr("Right thumb"), "rightThumb", page.remap.rightThumb),
        page.dpadRow(qsTr("D-pad up"), "dpadUp", page.remap.dpadUp),
        page.dpadRow(qsTr("D-pad down"), "dpadDown", page.remap.dpadDown),
        page.dpadRow(qsTr("D-pad left"), "dpadLeft", page.remap.dpadLeft),
        page.dpadRow(qsTr("D-pad right"), "dpadRight", page.remap.dpadRight)
    ]

    readonly property var stickRows: [
        page.axisRow(qsTr("Left stick X"), "leftStickX", page.remap.leftStickX),
        page.axisRow(qsTr("Left stick Y"), "leftStickY", page.remap.leftStickY),
        page.axisRow(qsTr("Right stick X"), "rightStickX", page.remap.rightStickX),
        page.axisRow(qsTr("Right stick Y"), "rightStickY", page.remap.rightStickY),
        page.triggerRow(qsTr("Left trigger"), "leftTrigger", page.remap.leftTrigger),
        page.triggerRow(qsTr("Right trigger"), "rightTrigger", page.remap.rightTrigger)
    ]

    // ---- Raw-input capture sink --------------------------------------------
    // The bridge filters to the capturing slot and rejects idle jitter, so a
    // resting pad never self-assigns.
    Connections {
        target: App

        function onRawInputCaptured(sid, kind, index, value) {
            if (sid !== page.slotId || page.capturingTarget.length === 0) {
                return;
            }
            App.assignSlotInput(page.slotId, page.capturingTarget, kind, index);
            page.cancelCapture();
            page.refresh();
        }
    }

    // ---- The assign row -----------------------------------------------------
    // One Component, two Repeaters: the row is written once, and it is not an
    // inline `component` type (a page declares no types).
    Component {
        id: assignRowDelegate

        RowLayout {
            id: assignRow

            required property var modelData

            readonly property bool armed: page.capturingTarget === assignRow.modelData.target

            Layout.fillWidth: true
            spacing: Tokens.s4

            AbstractButton {
                id: armButton

                Layout.fillWidth: true
                implicitHeight: Tokens.minTouch
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("%1 — %2").arg(assignRow.modelData.label)
                                                .arg(assignRow.modelData.value)
                Accessible.description: qsTr("Activate, then press the input to assign it.")

                onClicked: assignRow.armed
                           ? page.cancelCapture()
                           : page.beginCapture(assignRow.modelData.target,
                                               assignRow.modelData.label)

                HoverHandler { cursorShape: Qt.PointingHandCursor }

                background: Item {
                    Rectangle {
                        anchors.fill: parent
                        radius: Tokens.radiusChip
                        color: armButton.hovered ? Theme.primaryHover : "transparent"
                    }
                    // visualFocus only, so a mouse press never rings.
                    Rectangle {
                        anchors.fill: parent
                        visible: armButton.visualFocus
                        radius: Tokens.radiusChip
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.focusRing
                    }
                }

                contentItem: Item {
                    implicitHeight: Math.max(rowLabel.implicitHeight, valuePill.implicitHeight)

                    Label {
                        id: rowLabel
                        anchors.left: parent.left
                        anchors.right: valuePill.left
                        anchors.rightMargin: Tokens.s4
                        anchors.verticalCenter: parent.verticalCenter
                        text: assignRow.modelData.label
                        color: assignRow.armed ? Theme.primary : Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                        font.weight: assignRow.armed ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        id: valuePill
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: pillText.implicitWidth + Tokens.s8
                        implicitHeight: pillText.implicitHeight + Tokens.s3
                        width: implicitWidth
                        height: implicitHeight
                        radius: Tokens.radiusChip
                        color: Theme.surfaceDim
                        border.width: 1
                        border.color: assignRow.armed ? Theme.primary : Theme.outline

                        Label {
                            id: pillText
                            anchors.centerIn: parent
                            text: assignRow.armed ? qsTr("waiting for input…")
                                                  : assignRow.modelData.value
                            color: assignRow.armed ? Theme.primary : Theme.muted
                            font.family: Tokens.monoFamily
                            font.pixelSize: Tokens.textChip
                        }
                    }
                }
            }

            // Disabled rather than hidden when there is nothing to clear: a
            // control that appears on hover is a control a keyboard never finds.
            Kit.DishButton {
                size: Kit.DishButton.Small
                text: qsTr("Clear")
                enabled: assignRow.modelData.assigned === true
                Accessible.name: qsTr("Clear %1").arg(assignRow.modelData.label)
                Layout.alignment: Qt.AlignVCenter
                onClicked: page.clearAssignment(assignRow.modelData.target,
                                                assignRow.modelData.kind)
            }
        }
    }

    // ---- Body ---------------------------------------------------------------
    ColumnLayout {
        width: Math.min(page.bodyWidth, parent ? parent.width : 0)
        spacing: Tokens.s5

        // Not remappable: the ControllersPage entry gates on the `remappable`
        // role, but a slot can lose its identity while this page is open.
        Kit.EmptyState {
            visible: !page.hasRemap
            Layout.fillWidth: true
            glyph: "dish-off"
            title: qsTr("Not remappable")
            body: qsTr("This controller uses its built-in mapping and can’t be remapped here.")
        }

        // The capture card. 2px accent while armed — the one 2px border in the
        // app, and it marks an ARMED state, not a selection.
        Rectangle {
            visible: page.hasRemap
            Layout.fillWidth: true
            implicitHeight: captureColumn.implicitHeight + 2 * Tokens.s6
            radius: Tokens.radiusCard
            color: Theme.surface
            border.width: page.capturing ? 2 : 1
            border.color: page.capturing ? Theme.primary : Theme.outline

            ColumnLayout {
                id: captureColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Tokens.s7
                anchors.rightMargin: Tokens.s7
                spacing: Tokens.s3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Tokens.s7

                    Kit.DishProgressBar {
                        visible: page.capturing
                        indeterminate: true
                        Layout.preferredWidth: 80
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s1

                        Label {
                            text: page.capturing ? qsTr("Listening for input")
                                                 : qsTr("Click a control to rebind it")
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.StyledText
                            text: page.capturing
                                  ? qsTr("Press the input on your controller to assign <b>%1</b>. Idle jitter is ignored — a resting pad never self-assigns.").arg(page.capturingLabel)
                                  : qsTr("Then physically press or move the input on your controller to bind it. Esc cancels.")
                            color: Theme.muted
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }
                    }
                    Kit.DishButton {
                        visible: page.capturing
                        variant: Kit.DishButton.Outline
                        text: qsTr("Stop capture")
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: page.cancelCapture()
                    }
                }

                // Information the user must read: a colour, never an opacity.
                Label {
                    visible: page.captureExpired && !page.capturing
                    Layout.fillWidth: true
                    text: qsTr("No input seen — click the row to try again.")
                    color: Theme.mutedStrong
                    font.pixelSize: Tokens.textMeta
                    wrapMode: Text.WordWrap
                }
            }
        }

        GridLayout {
            visible: page.hasRemap
            Layout.fillWidth: true
            columns: page.availableWidth < Tokens.stackBreakpoint ? 1 : 2
            columnSpacing: Tokens.s6
            rowSpacing: Tokens.s6

            Kit.Card {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop

                contentItem: ColumnLayout {
                    spacing: Tokens.s1

                    Kit.Eyebrow {
                        mutedTone: true
                        text: qsTr("Buttons & D-pad")
                        Layout.bottomMargin: Tokens.s3
                    }

                    Repeater {
                        model: page.buttonRows
                        delegate: assignRowDelegate
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Tokens.s6

                Kit.Card {
                    Layout.fillWidth: true

                    contentItem: ColumnLayout {
                        spacing: Tokens.s1

                        Kit.Eyebrow {
                            mutedTone: true
                            text: qsTr("Sticks & triggers")
                            Layout.bottomMargin: Tokens.s3
                        }

                        Repeater {
                            model: page.stickRows
                            delegate: assignRowDelegate
                        }
                    }
                }

                Kit.Card {
                    Layout.fillWidth: true
                    dense: true

                    contentItem: ColumnLayout {
                        spacing: Tokens.s3

                        Kit.LabeledSwitch {
                            Layout.fillWidth: true
                            label: qsTr("Invert left stick Y")
                            checked: page.remap.invertLeftY === true
                            onToggled: checked => {
                                App.setSlotInvert(page.slotId, "leftY", checked);
                                page.refresh();
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: Theme.outline
                        }
                        Kit.LabeledSwitch {
                            Layout.fillWidth: true
                            label: qsTr("Invert right stick Y")
                            checked: page.remap.invertRightY === true
                            onToggled: checked => {
                                App.setSlotInvert(page.slotId, "rightY", checked);
                                page.refresh();
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Tokens.s5

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Stored for this controller model — applies on the next report, no re-attach.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                    Kit.DishButton {
                        variant: Kit.DishButton.Outline
                        text: qsTr("Reset to defaults")
                        onClicked: {
                            page.cancelCapture();
                            App.resetSlotRemap(page.slotId);
                            page.refresh();
                        }
                    }
                }
            }
        }
    }
}
