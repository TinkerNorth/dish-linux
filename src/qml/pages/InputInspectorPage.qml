// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller inspector: one pad's live input as it goes on the wire, the
// two stick tests, and the bench's feedback and audio tests, each offered only
// where the pad's path carries it. Pushed from Diagnostics with `slotId` set
// before load. Arming the inspector starts App's poll, so every way off this
// page disarms it.

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

    // Bumped on every state move: benchItems is a call, not a binding
    // dependency, and a path switch can change its answer.
    property int revision: 0
    readonly property var items: page.revision >= 0 ? App.benchItems(page.slotId) : []

    property int benchRevision: 0
    readonly property var bench: page.benchRevision >= 0 ? App.benchState(page.slotId) : ({})
    readonly property string running: page.bench.running || ""
    readonly property string asking: page.bench.asking || ""
    readonly property bool askingAboutSticks: page.asking.startsWith("stick.")

    property int outcomeRevision: 0
    readonly property var lastOutcome: page.outcomeRevision >= 0
                                       ? App.lastTestOutcome(page.slotId) : ({})

    Connections {
        target: App
        function onStateChanged() { page.revision += 1; }
        function onTestOutcomesChanged() { page.outcomeRevision += 1; }
        function onBenchChanged() { page.benchRevision += 1; }
    }

    function offers(item) {
        return page.items.indexOf(item) >= 0;
    }

    // The bench's rows, each test by its outcome name; a row shows only where
    // the pad's path carries its item.
    readonly property var benchRows: [
        { "item": "rumble", "title": qsTr("Rumble"), "tests": [
            { "test": "rumble.weak", "label": qsTr("Weak") },
            { "test": "rumble.strong", "label": qsTr("Strong") },
            { "test": "rumble.both", "label": qsTr("Both") } ] },
        { "item": "triggerRumble", "title": qsTr("Trigger rumble"), "tests": [
            { "test": "triggerRumble.left", "label": qsTr("Left") },
            { "test": "triggerRumble.right", "label": qsTr("Right") },
            { "test": "triggerRumble.both", "label": qsTr("Both") } ] },
        { "item": "lightbar", "title": qsTr("Light bar"), "tests": [
            { "test": "lightbar.cycle", "label": qsTr("Red, green, blue") } ] },
        { "item": "playerLeds", "title": qsTr("Player LEDs"), "tests": [
            { "test": "playerLeds.cycle", "label": qsTr("Each LED in turn") } ] },
        { "item": "adaptiveTriggers", "title": qsTr("Adaptive triggers"), "tests": [
            { "test": "trigger.pulse", "label": qsTr("Stiffen both") } ] },
        { "item": "micLamp", "title": qsTr("Mic lamp"), "tests": [
            { "test": "lamp.on", "label": qsTr("On") },
            { "test": "lamp.pulse", "label": qsTr("Pulse") } ] },
        { "item": "speaker", "title": qsTr("Speaker"), "tests": [
            { "test": "tone.play", "label": qsTr("Play the test tone") } ] },
        { "item": "haptics", "title": qsTr("HD haptics"), "tests": [
            { "test": "haptics.play", "label": qsTr("Play the tone on the haptics") } ] },
        { "item": "microphone", "title": qsTr("Microphone"), "tests": [
            { "test": "mic.level", "label": qsTr("Show its level") } ] }
    ].filter(row => page.offers(row.item))

    function testName(test) {
        switch (test) {
        case "rumble.weak":
            return qsTr("Weak rumble");
        case "rumble.strong":
            return qsTr("Strong rumble");
        case "rumble.both":
            return qsTr("Both motors");
        case "triggerRumble.left":
            return qsTr("Left trigger rumble");
        case "triggerRumble.right":
            return qsTr("Right trigger rumble");
        case "triggerRumble.both":
            return qsTr("Both trigger motors");
        case "lightbar.cycle":
            return qsTr("Light bar colours");
        case "playerLeds.cycle":
            return qsTr("Player LEDs");
        case "trigger.pulse":
            return qsTr("Adaptive triggers");
        case "lamp.on":
            return qsTr("Mic lamp on");
        case "lamp.pulse":
            return qsTr("Mic lamp pulsing");
        case "tone.play":
            return qsTr("Speaker tone");
        case "haptics.play":
            return qsTr("HD haptics tone");
        case "mic.level":
            return qsTr("Microphone level");
        case "stick.drift":
            return qsTr("Drift test");
        case "stick.range":
            return qsTr("Range test");
        }
        return test;
    }

    function lastOutcomeText() {
        if (page.lastOutcome.test === undefined)
            return "";
        const result = page.lastOutcome.result === "worked" ? qsTr("worked") : qsTr("did not work");
        return qsTr("Last test: %1 · %2 · %3").arg(page.testName(page.lastOutcome.test))
                                               .arg(result)
                                               .arg(Qt.formatTime(new Date(page.lastOutcome.atMs),
                                                                  "HH:mm:ss"));
    }

    function micLevelText() {
        return qsTr("Level %1% · peak %2%").arg(page.bench.micMeter || 0).arg(page.bench.micPeak || 0);
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

    function extendedButtonName(token) {
        switch (token) {
        case "rightPaddle1":
            return qsTr("Right paddle 1");
        case "leftPaddle1":
            return qsTr("Left paddle 1");
        case "rightPaddle2":
            return qsTr("Right paddle 2");
        case "leftPaddle2":
            return qsTr("Left paddle 2");
        case "misc1":
            return qsTr("Misc 1");
        case "leftPadClick":
            return qsTr("Left trackpad click");
        case "rightPadClick":
            return qsTr("Right trackpad click");
        case "leftTriggerClick":
            return qsTr("Left trigger click");
        case "rightTriggerClick":
            return qsTr("Right trigger click");
        case "leftStickTouch":
            return qsTr("Left stick touch");
        case "rightStickTouch":
            return qsTr("Right stick touch");
        case "leftGripTouch":
            return qsTr("Left grip touch");
        case "rightGripTouch":
            return qsTr("Right grip touch");
        }
        return token;
    }

    function pressedText() {
        const wire = (page.snapshot.buttons || []).map(token => page.buttonName(token));
        const extended = (page.snapshot.extButtons || []).map(token => page.extendedButtonName(token));
        const names = wire.concat(extended);
        return qsTr("Pressed: %1").arg(names.length > 0 ? names.join(" · ") : qsTr("none"));
    }

    function wheelText(step) {
        return qsTr("Wheel %1").arg(step > 0 ? "+" + step : String(step));
    }

    function pointerText() {
        const rightClick = page.snapshot.rightClick === true ? [qsTr("Right click")] : [];
        const wheel = page.snapshot.wheel ? [page.wheelText(page.snapshot.wheel)] : [];
        return rightClick.concat(wheel).join(" · ");
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
                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: page.pointerText()
                        color: Theme.onSurface
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

                    // The answer is the test's outcome: kept for this run and carried by the
                    // diagnostics file.
                    RowLayout {
                        Layout.fillWidth: true
                        visible: page.askingAboutSticks
                        spacing: Tokens.s4

                        Label {
                            text: qsTr("Did it work?")
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textSummary
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Yes")
                            onClicked: App.answerBenchTest(page.slotId, true)
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("No")
                            onClicked: App.answerBenchTest(page.slotId, false)
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        // ── Feedback and audio tests ─────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Feedback and audio tests") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s5

                    Label {
                        Layout.fillWidth: true
                        text: page.benchRows.length > 0
                              ? qsTr("Each test goes straight to the controller, with no host in the loop, whatever its switches say. Light, trigger and lamp tests put back what the game had set.")
                              : qsTr("This controller has nothing to test on the path it is on now.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: page.benchRows

                        delegate: ColumnLayout {
                            id: benchRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Tokens.s2

                            Label {
                                text: benchRow.modelData.title
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textSummary
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: Tokens.s4

                                Repeater {
                                    model: benchRow.modelData.tests

                                    delegate: Kit.OutlineButton {
                                        id: benchButton
                                        required property var modelData
                                        size: Kit.DishButton.Small
                                        text: benchButton.modelData.label
                                        enabled: page.running.length === 0
                                        onClicked: App.startBenchTest(page.slotId,
                                                                      benchButton.modelData.test)
                                    }
                                }
                                Kit.OutlineButton {
                                    size: Kit.DishButton.Small
                                    visible: benchRow.modelData.item === "microphone"
                                             && page.running === "mic.level"
                                    text: qsTr("Stop")
                                    onClicked: App.stopBenchTest(page.slotId)
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: benchRow.modelData.item === "microphone"
                                         && page.running === "mic.level"
                                text: page.micLevelText()
                                color: Theme.onSurface
                                font.family: Tokens.monoFamily
                                font.pixelSize: Tokens.textMeta
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: page.running.length > 0 && page.running !== "mic.level"
                                 && !page.running.startsWith("stick.")
                        text: qsTr("Testing: %1…").arg(page.testName(page.running))
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: (page.bench.unavailable || "").length > 0
                        text: qsTr("The controller's own audio device would not open, so nothing played or listened.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }

                    // The answer is the test's outcome: kept for this run and carried by the
                    // diagnostics file.
                    RowLayout {
                        Layout.fillWidth: true
                        visible: page.asking.length > 0 && !page.askingAboutSticks
                        spacing: Tokens.s4

                        Label {
                            text: qsTr("%1: did it work?").arg(page.testName(page.asking))
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textSummary
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Yes")
                            onClicked: App.answerBenchTest(page.slotId, true)
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("No")
                            onClicked: App.answerBenchTest(page.slotId, false)
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: page.lastOutcomeText()
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
