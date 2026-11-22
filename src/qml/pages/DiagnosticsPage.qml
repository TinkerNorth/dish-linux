// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Diagnostics: what the app sees right now. Each satellite session's own account
// of itself, each controller from its path down to the wire, and the flight
// recorder. Read-only: every figure is one the rest of the app already acts on,
// not a second measurement, and the words come from the tokens App vends.

// Bound: the card delegates read the outer `page` id and their required roles.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
// BindingDraft and LinkVocabulary resolve through Dish.Chrome; a `../shared`
// directory import would shadow them with second copies of the same files.
import Dish.Chrome
import "../kit" as Kit

Kit.Page {
    id: page
    title: qsTr("Diagnostics")

    readonly property string headerTitle: qsTr("Diagnostics")

    // var-typed so the shell's dynamic `shellApi` resolves without lint warnings.
    readonly property var shellView: StackView.view
    readonly property var shellApi: page.shellView ? page.shellView.shellApi : null

    // The key column every card's facts line up on.
    readonly property int keyColumn: 150
    // The log shows its newest lines; Copy takes all of them.
    readonly property int shownEvents: 30
    // The battery chip's wire status codes.
    readonly property int batteryCharging: 2
    readonly property int batteryFull: 3
    readonly property int batteryWired: 4

    // The reads below are invokable calls, which are not binding dependencies, so
    // these are bumped on every tick and state move that could change them.
    property int revision: 0
    property int logRevision: 0

    // The adapter can have been switched since the probe last ran.
    Component.onCompleted: App.refreshBluetoothState()

    Connections {
        target: App
        function onTelemetryChanged() { page.revision += 1; }
        function onStateChanged() { page.revision += 1; }
        function onDiagnosticsLogChanged() { page.logRevision += 1; }
        function onTestOutcomesChanged() { page.revision += 1; }
    }

    readonly property var hosts: page.revision >= 0 ? App.diagnosticsHosts() : []
    readonly property var events: page.logRevision >= 0 ? App.diagnosticsLog() : []
    readonly property var recentEvents: page.events.slice(-page.shownEvents).reverse()

    LinkVocabulary { id: linkWords }
    // The capability words, so a pad's advertised caps read as its table rows do.
    BindingDraft { id: featureWords }
    // LiveStat owns the one rate and latency formatter in the app.
    Kit.LiveStat { id: formatter; visible: false }

    function openInspector(slotId, slotName) {
        if (!page.shellApi)
            return;
        page.shellApi.pushDetail(Qt.resolvedUrl("InputInspectorPage.qml"),
                                 qsTr("Controller inspector"),
                                 { slotId: slotId, slotName: slotName });
    }

    // ── Hosts ────────────────────────────────────────────────────────────────

    function agreementText(token) {
        switch (token) {
        case "inStep":
            return qsTr("in step");
        case "diverged":
            return qsTr("out of step");
        }
        return qsTr("waiting for the first heartbeat");
    }

    function indexList(indices) {
        return indices.length > 0 ? indices.join(", ") : qsTr("none");
    }

    // A protocol only a negotiation settled: before one, the version is a default.
    function protocolText(host) {
        const settled = qsTr("v%1").arg(host.settledProtocol);
        const version = host.offeredProtocol === host.settledProtocol
            ? settled
            : qsTr("%1 (offered v%2)").arg(settled).arg(host.offeredProtocol);
        const standing = linkWords.compatText(host.compat);
        return standing.length > 0 ? version + " · " + standing : version;
    }

    function epochText(host) {
        const epochs = host.epoch === "unknown"
            ? qsTr("applied: %1").arg(host.appliedEpoch)
            : qsTr("on the host: %1 · applied: %2").arg(host.hostEpoch).arg(host.appliedEpoch);
        return epochs + " · " + page.agreementText(host.epoch);
    }

    function controllersText(host) {
        const lists = host.controllers === "unknown"
            ? qsTr("confirmed: %1").arg(page.indexList(host.confirmedControllers))
            : qsTr("on the host: %1 · confirmed: %2").arg(page.indexList(host.hostControllers))
                                                     .arg(page.indexList(host.confirmedControllers));
        return lists + " · " + page.agreementText(host.controllers);
    }

    function backendText(host) {
        const word = host.backend === "available" ? qsTr("available")
                   : host.backend === "unavailable" ? qsTr("unavailable")
                   : qsTr("unknown");
        return host.activeControllers >= 0
            ? word + " · " + qsTr("%n in use", "", host.activeControllers)
            : word;
    }

    function roundTripText(host) {
        if (host.rttSamples <= 0)
            return qsTr("no samples yet");
        const figures = qsTr("%1 p50 · %2 p99 · %3 one way")
            .arg(formatter.roundTripText(host.rttP50Ms, host.rttSamples))
            .arg(formatter.roundTripText(host.rttP99Ms, host.rttSamples))
            .arg(formatter.latencyText(host.oneWayMs, host.rttSamples));
        return figures + " · " + qsTr("%1 of %n pings", "", host.rttCapacity).arg(host.rttSamples);
    }

    function hostLines(host) {
        const lines = [{ "key": qsTr("Address"),
                         "value": qsTr("%1 • UDP %2").arg(host.ip).arg(host.udpPort) }];
        if (host.compat !== "unknown")
            lines.push({ "key": qsTr("Protocol"), "value": page.protocolText(host) });
        if (!host.live) {
            lines.push({ "key": qsTr("Session"), "value": qsTr("No session right now") });
            return lines;
        }
        lines.push({ "key": qsTr("Epoch"), "value": page.epochText(host) });
        lines.push({ "key": qsTr("Controllers"), "value": page.controllersText(host) });
        lines.push({ "key": qsTr("Virtual controllers"), "value": page.backendText(host) });
        lines.push({ "key": qsTr("Round trip"), "value": page.roundTripText(host) });
        lines.push({ "key": qsTr("Missed heartbeats"), "value": String(host.missedAcks) });
        lines.push({ "key": qsTr("Mouse control"),
                     "value": host.mouseControl ? qsTr("granted") : qsTr("not granted") });
        return lines;
    }

    // ── Controllers ──────────────────────────────────────────────────────────

    // A binding's type as its reasons name it, the way the binding editor names
    // it: a satellite type by its catalog name, a Moonlight type by the table.
    function typeNameOf(binding) {
        if (binding.hostKind !== "moonlight")
            return binding.typeName;
        return linkWords.moonlightTypeName(binding.solvedType, App.moonlightAutoType);
    }

    function transportText(pad) {
        if (pad.usbDirect)
            return qsTr("USB direct");
        return pad.bluetooth ? qsTr("Bluetooth") : qsTr("USB standard");
    }

    function phaseText(token) {
        switch (token) {
        case "routed":
            return qsTr("through the system driver");
        case "claiming":
            return qsTr("claiming");
        case "direct":
            return qsTr("claimed");
        case "awaitingFramework":
            return qsTr("waiting for the system to hand it back");
        case "restoreStuck":
            return qsTr("stuck returning to Standard");
        case "needsReplug":
            return qsTr("needs a replug");
        }
        return "";
    }

    function failureText(token) {
        switch (token) {
        case "permissionDenied":
            return qsTr("access denied");
        case "busy":
            return qsTr("held by another app");
        case "initFailed":
            return qsTr("sent nothing Dish could read");
        case "dropped":
            return qsTr("dropped during the claim");
        }
        return "";
    }

    // A pad the raw-HID path cannot claim has one path and nothing to report on it.
    function pathText(pad) {
        const choice = pad.desiredPath === "direct" ? qsTr("Direct") : qsTr("Standard");
        if (!pad.pathSupported)
            return choice;
        return [choice, page.phaseText(pad.pathPhase), page.failureText(pad.directFailure)]
            .filter(part => part.length > 0).join(" · ");
    }

    function rateText(pad) {
        const parts = [];
        if (pad.gamepadHzShown)
            parts.push(formatter.rateText(pad.gamepadHz, pad.gamepadHzLive));
        if (pad.pollHzShown)
            parts.push(qsTr("poll %1").arg(formatter.rateText(pad.pollHz, true)));
        if (pad.motionHzShown)
            parts.push(qsTr("motion %1").arg(formatter.rateText(pad.motionHz, true)));
        return parts.length > 0 ? parts.join(" · ") : qsTr("no reports yet");
    }

    function batteryText(pad) {
        switch (pad.batteryStatus) {
        case page.batteryWired:
            return qsTr("wired");
        case page.batteryFull:
            return qsTr("full");
        case page.batteryCharging:
            return qsTr("%1%, charging").arg(pad.batteryLevel);
        }
        return qsTr("%1%").arg(pad.batteryLevel);
    }

    function hostKindText(kind) {
        return kind === "moonlight" ? qsTr("Moonlight") : qsTr("Satellite");
    }

    function streamingText(token) {
        switch (token) {
        case "yes":
            return qsTr("yes");
        case "no":
            return qsTr("no");
        }
        return qsTr("waiting for the first heartbeat");
    }

    function advertisedText(tokens) {
        if (tokens.length === 0)
            return qsTr("nothing");
        return tokens.map(token => featureWords.featureName(token)).join(" · ");
    }

    function touchpadText(token) {
        switch (token) {
        case "ds4":
            return qsTr("pad");
        case "mouse":
            return qsTr("mouse");
        }
        return qsTr("off");
    }

    function padLines(pad) {
        const lines = [
            { "key": qsTr("Connection"), "value": page.transportText(pad) },
            { "key": qsTr("Path"), "value": page.pathText(pad) },
            { "key": qsTr("Input rate"), "value": page.rateText(pad) }
        ];
        if (pad.usbId.length > 0)
            lines.splice(1, 0, { "key": qsTr("USB ID"), "value": pad.usbId });
        if (pad.batteryKnown)
            lines.push({ "key": qsTr("Battery"), "value": page.batteryText(pad) });
        return lines.concat(page.bindingLines(pad.binding));
    }

    // The wire side is a satellite's: a Moonlight host declares a pad on arrival
    // and reports nothing back about it.
    function bindingLines(binding) {
        if (binding.bound !== true)
            return [{ "key": qsTr("Binding"), "value": qsTr("Not bound") }];
        const lines = [{ "key": qsTr("Binding"),
                         "value": binding.hostLabel + " · " + page.hostKindText(binding.hostKind) }];
        if (binding.hostKind === "moonlight") {
            lines.push({ "key": qsTr("On the wire"),
                         "value": qsTr("A Moonlight host reports nothing back about its controllers") });
            return lines;
        }
        if (!binding.declared) {
            lines.push({ "key": qsTr("On the wire"), "value": qsTr("Not declared to the host yet") });
            return lines;
        }
        lines.push({ "key": qsTr("Controller index"), "value": String(binding.controllerIndex) });
        lines.push({ "key": qsTr("Confirmed by host"),
                     "value": binding.confirmed ? qsTr("yes") : qsTr("no") });
        lines.push({ "key": qsTr("Streaming"), "value": page.streamingText(binding.streaming) });
        lines.push({ "key": qsTr("Advertised"), "value": page.advertisedText(binding.advertised) });
        lines.push({ "key": qsTr("Touchpad on the wire"),
                     "value": page.touchpadText(binding.touchpadMode) });
        return lines;
    }

    // ── The controller database and what it says about each pad ─────────────

    function chipText(token) {
        switch (token) {
        case "verified":
            return qsTr("Verified");
        case "flagged":
            return qsTr("Flagged");
        }
        return qsTr("Layout guessed");
    }

    function chipTone(token) {
        switch (token) {
        case "verified":
            return Kit.CapabilityChip.Present;
        case "flagged":
            return Kit.CapabilityChip.Absent;
        }
        return Kit.CapabilityChip.Warn;
    }

    function clockText(atMs) {
        return Qt.formatTime(new Date(atMs), "HH:mm:ss");
    }

    function recordText(record) {
        if (record.matched !== true)
            return qsTr("No record for this controller");
        const name = record.name.length > 0 ? record.name + " · " + record.recordId : record.recordId;
        return record.inert ? qsTr("%1 (name and notes only on this PC)").arg(name) : name;
    }

    function writeText(write) {
        if (write.route === undefined)
            return qsTr("nothing written yet");
        if (write.ok)
            return qsTr("sent · report %1 · %2").arg(write.reportId).arg(page.clockText(write.atMs));
        const reason = write.err.length > 0 ? write.err : qsTr("no error code");
        return qsTr("failed · report %1 · %2 · %3").arg(write.reportId).arg(reason)
                                                   .arg(page.clockText(write.atMs));
    }

    function testName(test) {
        switch (test) {
        case "rumble.weak":
            return qsTr("weak rumble");
        case "rumble.strong":
            return qsTr("strong rumble");
        case "rumble.both":
            return qsTr("both motors");
        case "triggerRumble.left":
            return qsTr("left trigger rumble");
        case "triggerRumble.right":
            return qsTr("right trigger rumble");
        case "triggerRumble.both":
            return qsTr("both trigger motors");
        case "lightbar.cycle":
            return qsTr("light bar colours");
        case "playerLeds.cycle":
            return qsTr("player LEDs");
        case "trigger.pulse":
            return qsTr("adaptive triggers");
        case "lamp.on":
            return qsTr("mic lamp on");
        case "lamp.pulse":
            return qsTr("mic lamp pulsing");
        case "tone.play":
            return qsTr("speaker tone");
        case "haptics.play":
            return qsTr("HD haptics tone");
        case "mic.level":
            return qsTr("microphone level");
        case "stick.drift":
            return qsTr("stick drift");
        case "stick.range":
            return qsTr("stick range");
        }
        return test;
    }

    function testText(outcome) {
        const result = outcome.result === "worked" ? qsTr("worked") : qsTr("did not work");
        return qsTr("%1 · %2 · %3").arg(page.testName(outcome.test)).arg(result)
                                   .arg(page.clockText(outcome.atMs));
    }

    function recordLines(pad, record) {
        const lines = [{ "key": qsTr("Record"), "value": page.recordText(record) }];
        if (pad.usbDirect && record.matched === true) {
            const check = record.mismatches.length > 0 ? record.mismatches.join("\n")
                                                       : qsTr("as the record expects");
            lines.push({ "key": qsTr("Hardware check"), "value": check });
        }
        if (pad.usbDirect)
            lines.push({ "key": qsTr("Last write"), "value": page.writeText(record.lastWrite) });
        if (record.lastTest.test !== undefined)
            lines.push({ "key": qsTr("Last test"), "value": page.testText(record.lastTest) });
        return lines;
    }

    function resultText(token) {
        switch (token) {
        case "upToDate":
            return qsTr("up to date");
        case "updated":
            return qsTr("updated");
        case "offline":
            return qsTr("could not reach dish.tinkernorth.com");
        case "invalid":
            return qsTr("the download did not verify");
        case "off":
            return qsTr("checks are off");
        }
        return "";
    }

    function databaseLines(db) {
        const active = db.source === "cache"
            ? qsTr("%1 · downloaded").arg(qsTr("v%1").arg(db.activeVersion))
            : qsTr("%1 · bundled").arg(qsTr("v%1").arg(db.activeVersion));
        const checked = db.lastCheckAtMs > 0
            ? qsTr("%1 · %2").arg(Qt.formatDateTime(new Date(db.lastCheckAtMs), "yyyy-MM-dd HH:mm"))
                             .arg(page.resultText(db.lastCheckResult))
            : qsTr("never");
        return [
            { "key": qsTr("In use"), "value": active },
            { "key": qsTr("Bundled"), "value": qsTr("v%1").arg(db.bundledVersion) },
            { "key": qsTr("Last check"), "value": db.checking ? qsTr("checking…") : checked }
        ];
    }

    // ── Sharing ──────────────────────────────────────────────────────────────

    // The mail's body: what this page shows, in the person's language, then what the file holds.
    function summaryText() {
        const db = App.controllerDb;
        const lines = [qsTr("Dish %1 on %2").arg(App.appVersion).arg(App.machineModel),
                       App.osVersion,
                       qsTr("Controller database: %1").arg(page.databaseLines(db)[0].value)];
        const pads = App.diagnosticsControllers();
        for (let i = 0; i < pads.length; ++i) {
            const recordWord = pads[i].recordId.length > 0
                ? pads[i].recordId + " · " + page.chipText(pads[i].chip)
                : qsTr("no record");
            lines.push(qsTr("%1: %2 · %3").arg(pads[i].name).arg(recordWord)
                                          .arg(page.transportText(pads[i])));
        }
        for (let h = 0; h < page.hosts.length; ++h) {
            lines.push(qsTr("Host %1: %2").arg(h + 1).arg(linkWords.chipText(page.hosts[h].chip)));
        }
        lines.push("");
        lines.push(qsTr("The attached file holds this page and its log, each controller's input and output counts, Dish's own view of each session and what each Satellite says about it; it never holds pairing keys, certificate fingerprints, IP addresses, Wi-Fi names or the names of your PCs and devices."));
        return lines.join("\n");
    }

    // "Report a problem" and "Ask a question" are one page in two categories.
    function openReport(category) {
        if (!page.shellApi)
            return;
        page.shellApi.pushDetail(Qt.resolvedUrl("ReportProblemPage.qml"),
                                 category === "question" ? qsTr("Ask a question")
                                                         : qsTr("Report a problem"),
                                 { category: category, summary: page.summaryText() });
    }

    function saveDiagnostics() {
        const path = App.saveDiagnostics();
        if (!page.shellApi)
            return;
        if (path.length > 0)
            page.shellApi.toast(qsTr("Diagnostics saved. The file manager shows its folder."), "success");
        else
            page.shellApi.toast(qsTr("The diagnostics file could not be written."), "error");
    }

    // ── Radios ───────────────────────────────────────────────────────────────

    function adapterText() {
        if (!App.bluetoothPresent)
            return qsTr("Not available");
        return App.bluetoothEnabled ? qsTr("On") : qsTr("Off");
    }

    // ── Events ───────────────────────────────────────────────────────────────

    function padPathText(token) {
        switch (token) {
        case "usbDirect":
            return qsTr("USB direct");
        case "bluetooth":
            return qsTr("Bluetooth");
        }
        return qsTr("USB standard");
    }

    function eventText(e) {
        switch (e.kind) {
        case "linkAppeared":
            return qsTr("%1: appeared, %2").arg(e.subject).arg(linkWords.chipText(e.to));
        case "linkChanged":
            return qsTr("%1: %2 → %3").arg(e.subject).arg(linkWords.chipText(e.from))
                                      .arg(linkWords.chipText(e.to));
        case "linkRemoved":
            return qsTr("%1: removed").arg(e.subject);
        case "padAttached":
            return qsTr("%1: attached (%2)").arg(e.subject).arg(page.padPathText(e.path));
        case "padDetached":
            return qsTr("%1: detached").arg(e.subject);
        case "padNeedsReplug":
            return qsTr("%1: needs a replug").arg(e.subject);
        case "padRestoreStuck":
            return qsTr("%1: stuck returning to Standard").arg(e.subject);
        case "padDirectFailed":
            return qsTr("%1: Direct claim failed (%2)").arg(e.subject)
                                                       .arg(page.failureText(e.failure));
        case "padBound":
            return qsTr("%1: bound to %2").arg(e.subject).arg(e.host);
        case "padUnbound":
            return qsTr("%1: unbound from %2").arg(e.subject).arg(e.host);
        }
        return e.subject;
    }

    function eventLine(e) {
        return Qt.formatTime(new Date(e.atMs), "HH:mm:ss") + "  " + page.eventText(e);
    }

    function copyEvents() {
        App.copyToClipboard(page.events.map(e => page.eventLine(e)).join("\n"));
        if (page.shellApi)
            page.shellApi.toast(qsTr("Events copied."), "success");
    }

    // One fact per line, key in the left column, shared by every card. A
    // Component rather than a type: a page declares no types of its own.
    Component {
        id: factLine

        RowLayout {
            id: line
            required property var modelData
            Layout.fillWidth: true
            spacing: Tokens.s6

            Label {
                Layout.preferredWidth: page.keyColumn
                Layout.alignment: Qt.AlignTop
                text: line.modelData.key
                color: Theme.muted
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                text: line.modelData.value
                color: Theme.onSurface
                font.family: Tokens.monoFamily
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
            }
        }
    }

    ColumnLayout {
        width: parent ? parent.width : implicitWidth
        spacing: Tokens.s8

        // ── Hosts ────────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Hosts") }

            Kit.Card {
                Layout.fillWidth: true
                visible: page.hosts.length === 0
                contentItem: Label {
                    text: qsTr("No satellites yet. Pair one on the Connections page and its session shows here.")
                    color: Theme.muted
                    font.pixelSize: Tokens.textSummary
                    wrapMode: Text.WordWrap
                }
            }

            Repeater {
                model: page.hosts

                delegate: Kit.Card {
                    id: hostCard
                    required property var modelData
                    Layout.fillWidth: true

                    readonly property var host: hostCard.modelData

                    contentItem: ColumnLayout {
                        spacing: Tokens.s3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.StatusDot { token: hostCard.host.dotColor }
                            Label {
                                Layout.fillWidth: true
                                text: hostCard.host.label
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textBase
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Kit.CapabilityChip {
                                text: linkWords.chipText(hostCard.host.chip)
                                tone: linkWords.chipTone(hostCard.host.chip)
                            }
                        }

                        Repeater {
                            model: page.hostLines(hostCard.host)
                            delegate: factLine
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: hostCard.host.live
                            spacing: Tokens.s6

                            Label {
                                Layout.preferredWidth: page.keyColumn
                                text: qsTr("Controller audio")
                                color: Theme.muted
                                font.pixelSize: Tokens.textMeta
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: Tokens.s2

                                Kit.CapabilityChip {
                                    text: featureWords.featureName("mic")
                                    tone: hostCard.host.hostMic ? Kit.CapabilityChip.Present
                                                                : Kit.CapabilityChip.Absent
                                }
                                Kit.CapabilityChip {
                                    text: featureWords.featureName("speaker")
                                    tone: hostCard.host.hostSpeaker ? Kit.CapabilityChip.Present
                                                                    : Kit.CapabilityChip.Absent
                                }
                                Kit.CapabilityChip {
                                    text: featureWords.featureName("hapticAudio")
                                    tone: hostCard.host.hostHaptics ? Kit.CapabilityChip.Present
                                                                    : Kit.CapabilityChip.Absent
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Controllers ──────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Controllers") }

            Kit.Card {
                Layout.fillWidth: true
                visible: App.slotCount === 0
                contentItem: Label {
                    text: qsTr("No controllers connected.")
                    color: Theme.muted
                    font.pixelSize: Tokens.textSummary
                    wrapMode: Text.WordWrap
                }
            }

            Repeater {
                model: App.slotModel

                delegate: Kit.Card {
                    id: pad
                    Layout.fillWidth: true

                    required property string slotId
                    required property string name
                    required property bool usbDirect
                    required property bool bluetooth
                    required property int gamepadHz
                    required property bool gamepadHzLive
                    required property bool gamepadHzShown
                    required property int motionHz
                    required property bool motionHzShown
                    required property int pollHz
                    required property bool pollHzShown
                    required property int batteryLevel
                    required property int batteryStatus
                    required property bool batteryKnown
                    required property string pathPhase
                    required property string desiredPath
                    required property bool pathSupported
                    required property string directFailure

                    readonly property var binding: page.revision >= 0
                                                   ? App.bindingDiagnostics(pad.slotId) : ({})
                    readonly property var record: page.revision >= 0
                                                  ? App.controllerRecordFacts(pad.slotId) : ({})
                    readonly property bool bound: pad.binding.bound === true

                    // The binding editor's explanations, so a diagnosed pad and an
                    // edited one never read two different reasons for one row.
                    BindingDraft {
                        id: explainer
                        slotId: pad.slotId
                        hostId: pad.bound ? pad.binding.hostId : ""
                        hostKind: pad.bound ? pad.binding.hostKind : "satellite"
                        desiredPath: pad.desiredPath === "direct" ? "direct" : "standard"
                        touchpadMode: pad.bound ? pad.binding.touchpadPick : 0
                        padName: pad.name
                        hostName: pad.bound ? pad.binding.hostLabel : ""
                        typeName: pad.bound ? page.typeNameOf(pad.binding) : ""
                        padClaimable: pad.pathSupported && !pad.bluetooth
                    }

                    readonly property var capabilityRows: pad.bound
                        ? explainer.annotate(pad.binding.capabilities, true) : []

                    Accessible.role: Accessible.ListItem
                    Accessible.name: pad.name

                    contentItem: ColumnLayout {
                        spacing: Tokens.s3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Label {
                                Layout.fillWidth: true
                                text: pad.name
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textBase
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Kit.CapabilityChip {
                                visible: pad.record.matched === true
                                text: page.chipText(pad.record.chip)
                                tone: page.chipTone(pad.record.chip)
                            }
                            Kit.OutlineButton {
                                size: Kit.DishButton.Small
                                text: qsTr("Inspect input")
                                onClicked: page.openInspector(pad.slotId, pad.name)
                            }
                        }

                        Repeater {
                            model: page.padLines(pad)
                            delegate: factLine
                        }

                        Repeater {
                            model: page.recordLines(pad, pad.record)
                            delegate: factLine
                        }

                        Repeater {
                            model: pad.record.matched === true ? pad.record.issues : []

                            delegate: Kit.Callout {
                                id: issueNote
                                required property var modelData
                                Layout.fillWidth: true
                                tone: Kit.Callout.Warning
                                text: issueNote.modelData.body.length > 0
                                      ? issueNote.modelData.title + "\n" + issueNote.modelData.body
                                      : issueNote.modelData.title
                            }
                        }

                        Kit.CapabilityTable {
                            Layout.fillWidth: true
                            Layout.topMargin: Tokens.s4
                            visible: pad.bound
                            rows: pad.capabilityRows
                        }
                    }
                }
            }
        }

        // ── Controller database ──────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Controller database") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s3

                    Repeater {
                        model: page.databaseLines(App.controllerDb)
                        delegate: factLine
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Tokens.s4
                        spacing: Tokens.s6

                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Check now")
                            enabled: App.controllerDbChecksEnabled && !App.controllerDb.checking
                            onClicked: App.checkControllerDbNow()
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: !App.controllerDbChecksEnabled
                            text: qsTr("Automatic controller checks are off in Settings, so Dish sends no request for the list.")
                            color: Theme.muted
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        // ── Sharing ──────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Send to support") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s4

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Report a problem writes a file of what this page shows and what each Satellite says about its side, and opens your own mail app addressed to support@tinkernorth.com with the file attached. Nothing is sent until you click Send.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s6

                        Kit.KitButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Report a problem")
                            onClicked: page.openReport("other")
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Ask a question")
                            onClicked: page.openReport("question")
                        }
                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Save diagnostics")
                            onClicked: page.saveDiagnostics()
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        // ── Radios ───────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Radios") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s3

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Bluetooth")
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textBase
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: [{ "key": qsTr("Adapter"), "value": page.adapterText() }]
                        delegate: factLine
                    }
                }
            }
        }

        // ── Events ───────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Events") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s2

                    Label {
                        Layout.fillWidth: true
                        visible: page.events.length === 0
                        text: qsTr("No events yet.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textSummary
                    }

                    Repeater {
                        model: page.recentEvents

                        delegate: Label {
                            id: eventRow
                            required property var modelData
                            Layout.fillWidth: true
                            text: page.eventLine(eventRow.modelData)
                            color: Theme.onSurface
                            font.family: Tokens.monoFamily
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Tokens.s4
                        spacing: Tokens.s6

                        Kit.OutlineButton {
                            size: Kit.DishButton.Small
                            text: qsTr("Copy events")
                            enabled: page.events.length > 0
                            onClicked: page.copyEvents()
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Newest first. The log covers this run of Dish and is not saved.")
                            color: Theme.muted
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}
