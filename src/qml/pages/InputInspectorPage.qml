// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller inspector: one pad's live input as it goes on the wire, the
// two stick tests, and a rumble test buzz. Pushed from Diagnostics with
// `slotId` set before load. Arming the inspector starts App's poll, so every
// way off this page disarms it.

// Bound: the stick delegates read the outer `page` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit

Kit.Page {
    id: page

    // Set by the pusher before this page becomes visible.
    property string slotId: ""
    property string slotName: ""

    title: qsTr("Controller inspector")
    readonly property string headerTitle: qsTr("Controller inspector")
    readonly property string headerSub: page.slotName

    readonly property var snapshot: App.inputSnapshot
    readonly property var stickTest: App.stickTest
    readonly property bool reporting: page.snapshot.hasReport === true
    readonly property bool testing: page.stickTest.phase === "running"

    // Bumped on every state move: canTestRumble is a call, not a binding
    // dependency, and a path switch can change its answer.
    property int revision: 0
    readonly property bool canBuzz: page.revision >= 0 && App.canTestRumble(page.slotId)

    Connections {
        target: App
        function onStateChanged() { page.revision += 1; }
    }

    Component.onCompleted: App.startInputInspection(page.slotId)
    Component.onDestruction: App.stopInputInspection()

    function percent(fraction) {
        return qsTr("%1%").arg(Math.round(fraction * 100));
    }

    function buttonName(token) {
        switch (token) {
        case "dpadUp":
            return "▲";
        case "dpadDown":
            return "▼";
        case "dpadLeft":
            return "◀";
        case "dpadRight":
            return "▶";
        case "start":
            return qsTr("Start");
        case "back":
            return qsTr("Back");
        case "leftThumb":
            return "LS";
        case "rightThumb":
            return "RS";
        case "leftShoulder":
            return "LB";
        case "rightShoulder":
            return "RB";
        case "guide":
            return qsTr("Guide");
        case "micMute":
            return qsTr("Mute");
        case "a":
            return "A";
        case "b":
            return "B";
        case "x":
            return "X";
        case "y":
            return "Y";
        }
        return token;
    }

    function pressedText() {
        const buttons = page.snapshot.buttons || [];
        const names = buttons.map(token => page.buttonName(token));
        return qsTr("Pressed: %1").arg(names.length > 0 ? names.join(" · ") : qsTr("none"));
    }

    function fingerText(active, x, y) {
        return active ? qsTr("%1 across, %2 down").arg(page.percent(x)).arg(page.percent(y))
                      : qsTr("lifted");
    }

    function stickTestText() {
        const test = page.stickTest;
        if (test.phase === "running")
            return qsTr("Capturing… %1 s").arg(test.secondsLeft);
        if (test.phase !== "done")
            return "";
        if (test.samples === 0)
            return qsTr("The controller sent nothing during the test.");
        if (test.kind === "drift")
            return qsTr("Resting drift: L %1 · R %2. Suggested dead zone: %3.")
                .arg(page.percent(test.driftLeft)).arg(page.percent(test.driftRight))
                .arg(page.percent(test.suggestedDeadzone));
        return qsTr("Reach: L %1 · R %2. Circularity error: L %3 · R %4.")
            .arg(page.percent(test.reachLeft)).arg(page.percent(test.reachRight))
            .arg(test.circularityLeftKnown ? page.percent(test.circularityLeft) : qsTr("n/a"))
            .arg(test.circularityRightKnown ? page.percent(test.circularityRight) : qsTr("n/a"));
    }

    readonly property var sticks: [
        { "name": qsTr("Left stick"), "x": page.snapshot.lx || 0, "y": page.snapshot.ly || 0 },
        { "name": qsTr("Right stick"), "x": page.snapshot.rx || 0, "y": page.snapshot.ry || 0 }
    ]

    readonly property var triggers: [
        { "name": qsTr("Left trigger"), "value": page.snapshot.lt || 0 },
        { "name": qsTr("Right trigger"), "value": page.snapshot.rt || 0 }
    ]

    ColumnLayout {
        width: parent ? parent.width : implicitWidth
        spacing: Tokens.s8

        // ── Live input ───────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Live input") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s6

                    Label {
                        Layout.fillWidth: true
                        visible: !page.reporting
                        text: qsTr("Waiting for this controller's first report. Move a stick or press a button.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textSummary
                        wrapMode: Text.WordWrap
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: Tokens.s10

                        Repeater {
                            model: page.sticks

                            delegate: ColumnLayout {
                                id: stick
                                required property var modelData
                                spacing: Tokens.s2

                                Kit.StickPlot {
                                    Layout.alignment: Qt.AlignHCenter
                                    stickX: stick.modelData.x
                                    stickY: stick.modelData.y
                                    reporting: page.reporting
                                    accessibleName: stick.modelData.name
                                }
                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: stick.modelData.name
                                    color: Theme.muted
                                    font.pixelSize: Tokens.textMeta
                                }
                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: stick.modelData.x.toFixed(2) + ", " + stick.modelData.y.toFixed(2)
                                    color: Theme.onSurface
                                    font.family: Tokens.monoFamily
                                    font.pixelSize: Tokens.textMeta
                                }
                            }
                        }

                        ColumnLayout {
                            spacing: Tokens.s4

                            Repeater {
                                model: page.triggers

                                delegate: ColumnLayout {
                                    id: trigger
                                    required property var modelData
                                    spacing: Tokens.s1

                                    Label {
                                        text: qsTr("%1: %2").arg(trigger.modelData.name)
                                                            .arg(page.percent(trigger.modelData.value))
                                        color: Theme.muted
                                        font.pixelSize: Tokens.textMeta
                                    }
                                    Kit.DishProgressBar {
                                        Layout.preferredWidth: Tokens.glyphHero * 2
                                        indeterminate: false
                                        value: trigger.modelData.value
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: page.pressedText()
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("This is the input as it goes to the host, after the dead zones.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        // ── Motion and touch ─────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Motion and touch") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s3

                    Label {
                        Layout.fillWidth: true
                        text: page.snapshot.hasMotion === true
                              ? qsTr("Gyro: %1 · %2 · %3 deg/s")
                                    .arg(page.snapshot.gyroX.toFixed(1))
                                    .arg(page.snapshot.gyroY.toFixed(1))
                                    .arg(page.snapshot.gyroZ.toFixed(1))
                              : qsTr("No motion data from this controller.")
                        color: page.snapshot.hasMotion === true ? Theme.onSurface : Theme.muted
                        font.family: Tokens.monoFamily
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: page.snapshot.hasMotion === true
                        text: page.snapshot.hasMotion === true
                              ? qsTr("Accel: %1 · %2 · %3 g")
                                    .arg(page.snapshot.accelX.toFixed(2))
                                    .arg(page.snapshot.accelY.toFixed(2))
                                    .arg(page.snapshot.accelZ.toFixed(2))
                              : ""
                        color: Theme.onSurface
                        font.family: Tokens.monoFamily
                        font.pixelSize: Tokens.textMeta
                    }
                    Label {
                        Layout.fillWidth: true
                        text: page.snapshot.hasTouch === true
                              ? qsTr("Finger 1: %1 · Finger 2: %2 · Click: %3")
                                    .arg(page.fingerText(page.snapshot.finger0, page.snapshot.finger0X,
                                                         page.snapshot.finger0Y))
                                    .arg(page.fingerText(page.snapshot.finger1, page.snapshot.finger1X,
                                                         page.snapshot.finger1Y))
                                    .arg(page.snapshot.touchClick ? qsTr("pressed") : qsTr("released"))
                              : qsTr("No touch data from this controller.")
                        color: page.snapshot.hasTouch === true ? Theme.onSurface : Theme.muted
                        font.family: Tokens.monoFamily
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        // ── Stick tests ──────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Stick tests") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s5

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The tests read the sticks as the controller sends them, before the dead zones.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Drift test (hands off the sticks)")
                            enabled: !page.testing
                            onClicked: App.startStickTest("drift")
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Range test (sweep full circles)")
                            enabled: !page.testing
                            onClicked: App.startStickTest("range")
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: page.stickTestText()
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        // ── Rumble test ──────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Rumble test") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s5

                    Label {
                        Layout.fillWidth: true
                        text: page.canBuzz
                              ? qsTr("A short buzz straight to the controller, whatever its rumble switch says.")
                              : qsTr("This controller has no rumble motors on the path it is on now.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }

                    Flow {
                        Layout.fillWidth: true
                        visible: page.canBuzz
                        spacing: Tokens.s4

                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Weak")
                            onClicked: App.testRumble(page.slotId, "weak")
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Strong")
                            onClicked: App.testRumble(page.slotId, "strong")
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Both")
                            onClicked: App.testRumble(page.slotId, "both")
                        }
                    }
                }
            }
        }
    }
}
