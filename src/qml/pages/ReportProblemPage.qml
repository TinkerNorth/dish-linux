// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// "Report a problem" and "Ask a question": one page, two starting categories
// (docs/diagnostics-bundle.md, "The report"). The person picks a category,
// writes what happened and may name the controller; the page says in plain
// words what the file will hold, shows the file on request, and hands the mail
// to their own mail app. Pushed from Diagnostics with `category` and `summary`
// (the Diagnostics page's own summary, in the person's language) set before
// load. Opening it asks every live Satellite for its side of the session.

// Bound: the row delegates read the outer `page` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit

Kit.Page {
    id: page

    // Set by the pusher before this page becomes visible.
    property string category: "other"
    property string summary: ""

    title: page.asking ? qsTr("Ask a question") : qsTr("Report a problem")
    readonly property string headerTitle: page.asking ? qsTr("Ask a question") : qsTr("Report a problem")

    readonly property bool asking: page.category === "question"

    // The contract's order; the words are the person's language, the tokens the file's.
    readonly property var categoryTokens: ["notRecognised", "noFeedback", "connection", "audio",
                                           "other", "question"]
    readonly property var categoryWords: [qsTr("Controller not recognised"),
                                          qsTr("No rumble or feedback"),
                                          qsTr("Connection or latency"),
                                          qsTr("Audio"),
                                          qsTr("Something else"),
                                          qsTr("A question")]
    readonly property string categoryText: page.categoryWords[Math.max(0, page.categoryTokens.indexOf(page.category))]

    readonly property var pads: App.diagnosticsControllers()
    // An attached pad is named from the start; "" is no particular controller.
    property string slotId: page.pads.length > 0 ? page.pads[0].slotId : ""

    property bool previewing: false
    // The last file handed to the mail app, which the file manager now shows; "" before the first.
    property string handedFileName: ""
    property string previewText: ""

    // "Capture this controller", offered for a pad Dish reads directly: its descriptors and the
    // reports it sends at rest ride the file, for the controller the report names only.
    readonly property bool pickedDirect: page.padIsDirect(page.slotId)
    readonly property string captureState: App.controllerCapture.slotId === page.slotId
                                           ? App.controllerCapture.state : "idle"
    readonly property string captureWords: page.captureState === "capturing"
        ? qsTr("Recording its reports. Leave the controller untouched…")
        : page.captureState === "captured"
          ? qsTr("Captured: the file now holds this controller's descriptors and the reports it sent while it lay still.")
          : page.captureState === "failed"
            ? qsTr("The capture did not finish: Dish no longer reads this controller directly.")
            : qsTr("Dish can add this controller's USB descriptors and twenty reports taken while it lies still. Put it down, leave it untouched, then capture.")

    Component.onCompleted: App.prepareReport()

    Connections {
        target: App
        function onDiagnosticsMailHandedOff(viaMailClient, fileName) {
            page.handedFileName = fileName;
            if (!page.shellApi)
                return;
            page.shellApi.toast(viaMailClient
                                ? qsTr("Your mail app has the message. Nothing is sent until you click Send.")
                                : qsTr("Your mail app opened without the file: attach the one the file manager shows."),
                                viaMailClient ? "success" : "warning");
        }
        function onReportReadyChanged() {
            if (page.previewing && App.reportReady)
                page.refreshPreview();
        }
    }

    // var-typed so the shell's dynamic `shellApi` resolves without lint warnings.
    readonly property var shellView: StackView.view
    readonly property var shellApi: page.shellView ? page.shellView.shellApi : null

    function padIsDirect(slotId) {
        for (let i = 0; i < page.pads.length; ++i) {
            if (page.pads[i].slotId === slotId)
                return page.pads[i].usbDirect;
        }
        return false;
    }

    function pick(words) {
        const at = page.categoryWords.indexOf(words);
        if (at >= 0)
            page.category = page.categoryTokens[at];
    }

    function refreshPreview() {
        page.previewText = App.reportPreview(page.category, reportText.text, page.slotId);
    }

    function togglePreview() {
        page.previewing = !page.previewing;
        if (page.previewing)
            page.refreshPreview();
    }

    function sendReport() {
        if (App.sendReport(page.category, page.categoryText, reportText.text, page.slotId, page.summary))
            return;
        if (page.shellApi)
            page.shellApi.toast(qsTr("The diagnostics file could not be written, or a mail from the last one is still open."), "error");
    }

    ColumnLayout {
        width: parent ? parent.width : implicitWidth
        spacing: Tokens.s8

        Label {
            Layout.fillWidth: true
            text: page.asking
                  ? qsTr("Ask us anything about Dish. Your question goes to support@tinkernorth.com from your own mail app, with a file of what Dish sees so we can answer for your setup.")
                  : qsTr("Tell us what went wrong. Dish writes a file of what it sees and opens your own mail app addressed to support@tinkernorth.com, with the file attached.")
            color: Theme.muted
            font.pixelSize: Tokens.textMeta
            wrapMode: Text.WordWrap
        }

        // ── What it is about ─────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Category") }

            Kit.ComboButton {
                Layout.fillWidth: true
                options: page.categoryWords
                value: page.categoryText
                onPicked: (option) => page.pick(option)
            }

            Kit.SectionHeader { label: qsTr("What happened, and what did you expect?") }

            Kit.KitTextArea {
                id: reportText
                Layout.fillWidth: true
                placeholderText: qsTr("What happened, and what did you expect?")
                onTextChanged: {
                    if (page.previewing)
                        page.refreshPreview();
                }
            }
        }

        // ── The controller ───────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Controller") }

            Repeater {
                model: page.pads
                delegate: Kit.SelectRow {
                    id: padRow
                    required property var modelData
                    Layout.fillWidth: true
                    selected: page.slotId === padRow.modelData.slotId
                    title: padRow.modelData.name
                    subtitle: padRow.modelData.recordId
                    onPicked: page.slotId = padRow.modelData.slotId
                }
            }

            Kit.SelectRow {
                Layout.fillWidth: true
                selected: page.slotId === ""
                title: qsTr("No particular controller")
                onPicked: page.slotId = ""
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: page.pickedDirect
                spacing: Tokens.s3

                Label {
                    Layout.fillWidth: true
                    text: page.captureWords
                    color: Theme.muted
                    font.pixelSize: Tokens.textMeta
                    wrapMode: Text.WordWrap
                }
                Kit.OutlineButton {
                    size: Kit.DishButton.Small
                    text: qsTr("Capture this controller")
                    enabled: page.captureState !== "capturing"
                    onClicked: App.captureController(page.slotId)
                }
            }
        }

        // ── The file ─────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("What the file holds") }

            Kit.Card {
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    spacing: Tokens.s3

                    Repeater {
                        model: [qsTr("Your words and the category you picked."),
                                qsTr("Dish's version, this PC's model and Linux version, its battery and power profile, and how hot it runs."),
                                qsTr("Each controller's identity, the path Dish reads it on, and what went in and out of it, as counts and last values."),
                                qsTr("Each host's kind, version and link state, Dish's own view of each session, and what each Satellite says about it."),
                                qsTr("The controller database's state and this run's event log."),
                                qsTr("For a controller you captured: its USB descriptors and the reports it sent while it lay still.")]
                        delegate: Label {
                            id: holdsLine
                            required property string modelData
                            Layout.fillWidth: true
                            text: "• " + holdsLine.modelData
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: Tokens.s2
                        text: qsTr("It never holds pairing keys, certificate fingerprints, IP addresses, Wi-Fi names or the names of your PCs and devices. Nothing is sent until you click Send in your mail app.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: page.handedFileName !== ""
                text: qsTr("Your mail should carry the file %1. If it does not, attach it from the folder that just opened.").arg(page.handedFileName)
                color: Theme.onSurface
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: !App.reportReady
                text: qsTr("Asking each Satellite for its side of the session…")
                color: Theme.muted
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Tokens.s6

                Kit.KitButton {
                    size: Kit.DishButton.Small
                    text: qsTr("Continue in your mail app")
                    enabled: App.reportReady
                    onClicked: page.sendReport()
                }
                Kit.OutlineButton {
                    size: Kit.DishButton.Small
                    text: page.previewing ? qsTr("Hide file") : qsTr("Preview file")
                    enabled: App.reportReady
                    onClicked: page.togglePreview()
                }
                Item { Layout.fillWidth: true }
            }

            Kit.Card {
                Layout.fillWidth: true
                visible: page.previewing
                contentItem: TextEdit {
                    readOnly: true
                    selectByMouse: true
                    text: page.previewText
                    color: Theme.onSurface
                    selectionColor: Theme.primary
                    selectedTextColor: Theme.onPrimary
                    font.family: Tokens.monoFamily
                    font.pixelSize: Tokens.textMeta
                    wrapMode: TextEdit.WrapAnywhere
                }
            }
        }
    }
}
