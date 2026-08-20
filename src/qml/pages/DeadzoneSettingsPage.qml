// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Dead zones & motion detail, one card per device.
//
// The sliders PRESENT percentages of full axis travel while the persisted values
// stay in the raw units the input processor consumes (stick flat 0..32767,
// trigger flat 0..255) — the mapping lives here so neither the store schema nor
// the hot path changes.
//
// A drag must not hammer the repository: `moved` keeps the tuning live but
// rate-limited to one push per Tokens.durNormal; `committed` (release) is the
// authoritative write.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: deadzonePage

    readonly property string headerTitle: qsTr("Dead zones & motion")
    readonly property string headerSub: qsTr("Per-device tuning — applied live")

    readonly property real stickRange: 32767
    readonly property real triggerRange: 255
    readonly property int percentMax: 30

    readonly property int bodyWidth: 640

    function toPercent(raw, range) { return Math.round(raw / range * 100); }
    function fromPercent(pct, range) { return Math.round(pct / 100 * range); }

    property var deviceModel: App.deadzoneDevices()

    // The gesture in flight. Only one slider can be dragged at a time, so one
    // pending triple is enough.
    property string pendingDeviceId: ""
    property int pendingStick: 0
    property int pendingTrigger: 0

    Timer {
        id: livePush
        interval: Tokens.durNormal
        repeat: false
        onTriggered: deadzonePage.flush()
    }

    function pushLive(deviceId, stickPct, triggerPct) {
        deadzonePage.pendingDeviceId = deviceId;
        deadzonePage.pendingStick = stickPct;
        deadzonePage.pendingTrigger = triggerPct;
        if (!livePush.running) {
            livePush.start();
        }
    }

    // Supersedes any pending live push so the gesture ends on exactly one value.
    function commit(deviceId, stickPct, triggerPct) {
        livePush.stop();
        deadzonePage.pendingDeviceId = deviceId;
        deadzonePage.pendingStick = stickPct;
        deadzonePage.pendingTrigger = triggerPct;
        deadzonePage.flush();
    }

    function flush() {
        if (deadzonePage.pendingDeviceId.length === 0) {
            return;
        }
        App.setDeadzones(deadzonePage.pendingDeviceId,
                         deadzonePage.fromPercent(deadzonePage.pendingStick,
                                                  deadzonePage.stickRange),
                         deadzonePage.fromPercent(deadzonePage.pendingTrigger,
                                                  deadzonePage.triggerRange));
    }

    Connections {
        target: App
        function onDeadzonesChanged() { deadzonePage.deviceModel = App.deadzoneDevices(); }
    }

    ColumnLayout {
        width: Math.min(deadzonePage.bodyWidth, parent ? parent.width : 0)
        spacing: Tokens.s6

        Kit.EmptyState {
            visible: deadzonePage.deviceModel.length === 0
            Layout.fillWidth: true
            glyph: "dish-off"
            title: qsTr("No controllers connected")
            body: qsTr("Connect a controller to adjust its dead zones.")
        }

        Repeater {
            model: deadzonePage.deviceModel

            delegate: Kit.Card {
                id: deviceCard
                required property var modelData

                readonly property string deviceId: deviceCard.modelData.id
                readonly property bool hasGyro: deviceCard.modelData.hasGyro === true
                readonly property int stickPct: deadzonePage.toPercent(
                    deviceCard.modelData.stickFlat, deadzonePage.stickRange)
                readonly property int triggerPct: deadzonePage.toPercent(
                    deviceCard.modelData.triggerFlat, deadzonePage.triggerRange)

                Layout.fillWidth: true

                contentItem: ColumnLayout {
                    spacing: Tokens.s3

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s5

                        Label {
                            Layout.fillWidth: true
                            text: deviceCard.modelData.name.length > 0
                                  ? deviceCard.modelData.name : deviceCard.deviceId
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Kit.CapabilityChip {
                            tone: deviceCard.hasGyro ? Kit.CapabilityChip.Present
                                                     : Kit.CapabilityChip.Absent
                            text: deviceCard.hasGyro ? qsTr("Gyro") : qsTr("No gyro")
                        }
                        // The repository's own default is a zero flat on both axes.
                        Kit.DishButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Reset")
                            Accessible.name: qsTr("Reset dead zones for %1")
                                             .arg(deviceCard.modelData.name)
                            onClicked: deadzonePage.commit(deviceCard.deviceId, 0, 0)
                        }
                    }

                    GridLayout {
                        id: tuningGrid
                        Layout.fillWidth: true
                        Layout.topMargin: Tokens.s3
                        columns: 2
                        columnSpacing: Tokens.s10

                        // Layouts split surplus from the PREFERRED widths, so
                        // without an equal preference the column boundary would
                        // follow the label lengths, then twitch as the live %
                        // readout changes width.
                        readonly property int cellWidth:
                            (deadzonePage.bodyWidth - 2 * Tokens.s7 - Tokens.s10) / 2

                        Kit.SliderRow {
                            Layout.fillWidth: true
                            Layout.preferredWidth: tuningGrid.cellWidth
                            label: qsTr("Stick dead zone")
                            maxValue: deadzonePage.percentMax
                            value: deviceCard.stickPct
                            onMoved: pct => deadzonePage.pushLive(deviceCard.deviceId, pct,
                                                                  deviceCard.triggerPct)
                            onCommitted: pct => deadzonePage.commit(deviceCard.deviceId, pct,
                                                                    deviceCard.triggerPct)
                        }
                        Kit.SliderRow {
                            Layout.fillWidth: true
                            Layout.preferredWidth: tuningGrid.cellWidth
                            label: qsTr("Trigger dead zone")
                            maxValue: deadzonePage.percentMax
                            value: deviceCard.triggerPct
                            onMoved: pct => deadzonePage.pushLive(deviceCard.deviceId,
                                                                  deviceCard.stickPct, pct)
                            onCommitted: pct => deadzonePage.commit(deviceCard.deviceId,
                                                                    deviceCard.stickPct, pct)
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: Tokens.s3
                        implicitHeight: 1
                        color: Theme.outline
                    }

                    Kit.LabeledSwitch {
                        Layout.fillWidth: true
                        visible: deviceCard.hasGyro
                        label: qsTr("Forward motion (gyro)")
                        description: qsTr("Send gyroscope and accelerometer readings to the host.")
                        checked: deviceCard.modelData.forwardMotion === true
                        onToggled: checked => App.setMotionEnabled(deviceCard.deviceId, checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: Tokens.s2
                        Layout.bottomMargin: Tokens.s2
                        visible: !deviceCard.hasGyro
                        text: qsTr("Motion forwarding unavailable — this device has no gyroscope.")
                        color: Theme.mutedStrong
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Overrides are stored per device and take effect immediately.")
            color: Theme.muted
            font.pixelSize: Tokens.textMeta
            wrapMode: Text.WordWrap
        }
    }
}
