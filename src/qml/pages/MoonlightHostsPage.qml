// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Moonlight destination — the inventory of GameStream HOSTS (Sunshine,
// Apollo, Wolf), the sibling of ConnectionsPage's satellite inventory. It is
// deliberately a SEPARATE page rather than a second section of Connections: the
// two host kinds pair differently, carry different capabilities, and a merged
// list would make "Pair" mean two different things in one column.
//
// PAIRING IS NOT CONNECTING, and this page never claims otherwise. Moonlight
// has no bidirectional liveness, so there is no light to draw: a row states the
// trust it remembers and whether this visit confirmed it, re-asked on open and
// never polled. What runs, and for whom, is the BINDING flow's question; this
// page owns pairing, forgetting, and the one escape hatch that ends a session
// from outside a binding.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: page
    title: qsTr("Moonlight hosts")

    readonly property string headerTitle: qsTr("Moonlight hosts")
    readonly property string headerSub: page.pairedCount === 0
        ? qsTr("%n found", "", page.hostRows.length)
        : qsTr("%n paired", "", page.pairedCount)
    readonly property string headerDot: page.pairedCount === 0 ? "muted" : "success"

    // Where a user without a host goes to get one.
    readonly property string sunshineUrl: "https://github.com/LizardByte/Sunshine"

    readonly property var hostRows: App.moonlightHosts

    property string currentHostId: ""
    property string currentLabel: ""
    property bool currentHasSession: false

    readonly property int pairedCount: {
        let n = 0;
        for (let i = 0; i < page.hostRows.length; ++i) {
            if (page.hostRows[i].trust === "paired" || page.hostRows[i].trust === "remembered")
                n += 1;
        }
        return n;
    }

    // Trust is verified LAZILY: on entering this screen, and again before a
    // session starts. Never on a timer — the host would not answer a question
    // nobody asked, and a poll would only invent a liveness it cannot report.
    Component.onCompleted: {
        if (!App.moonlightScanning)
            App.scanMoonlight();
        page.reprobeAll();
    }

    function reprobeAll() {
        for (let i = 0; i < page.hostRows.length; ++i)
            App.probeMoonlightHost(page.hostRows[i].uuid);
    }

    ColumnLayout {
        width: parent.width
        spacing: Tokens.s5

        // ---- FOUND + manual add ---------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { glyph: "dish-logo"; label: qsTr("Found") }
            Item { Layout.fillWidth: true }
            Kit.LiveStat {
                live: App.moonlightScanning
                text: App.moonlightScanning ? qsTr("scanning…")
                                            : qsTr("%n found", "", page.hostRows.length)
            }
            Kit.DishButton {
                text: qsTr("Add by address…")
                variant: Kit.DishButton.Outline
                onClicked: addSheet.open()
            }
            Kit.DishButton {
                text: App.moonlightScanning ? qsTr("Scanning…") : qsTr("Scan")
                variant: Kit.DishButton.Outline
                enabled: !App.moonlightScanning
                onClicked: App.scanMoonlight()
            }
        }

        Kit.DishProgressBar {
            visible: App.moonlightScanning
            indeterminate: true
            Layout.fillWidth: true
        }

        // Empty is a real state, and it says so differently while a sweep runs.
        Kit.EmptyState {
            visible: page.hostRows.length === 0 && App.moonlightScanning
            glyph: "satellite-broadcasting"
            title: qsTr("Looking for Moonlight hosts")
            body: qsTr("Scanning your network for hosts advertising GameStream. They appear here as they answer.")
            showAction: false
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s5
            Layout.bottomMargin: Tokens.s5
        }

        Kit.EmptyState {
            visible: page.hostRows.length === 0 && !App.moonlightScanning
            glyph: "satellite-off"
            title: qsTr("No Moonlight hosts found")
            body: qsTr("A PC appears here once Sunshine, Apollo or Wolf is running on it and both machines are on the same network. You can also add one by address.")
            showAction: false
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s5
            Layout.bottomMargin: Tokens.s5

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Tokens.s3
                spacing: Tokens.s4

                Kit.DishButton {
                    text: qsTr("Get Sunshine ↗")
                    variant: Kit.DishButton.Outline
                    onClicked: App.openExternalUrl(page.sunshineUrl)
                }
                Kit.DishButton {
                    text: qsTr("Add by address…")
                    variant: Kit.DishButton.Outline
                    onClicked: addSheet.open()
                }
            }
        }

        // ---- One card per host ----------------------------------------------
        Repeater {
            model: page.hostRows

            delegate: Kit.Card {
                id: host
                required property var modelData

                readonly property string hostId: host.modelData.uuid
                readonly property string label: host.modelData.name
                readonly property string trust: host.modelData.trust
                readonly property string phase: host.modelData.phase
                readonly property int controllers: host.modelData.controllers
                readonly property bool sessionUp: host.phase === "streaming"
                                                  || host.phase === "faltering"
                                                  || host.phase === "launching"
                                                  || host.phase === "connecting"
                readonly property bool busy: host.phase === "pairing"
                                             || host.phase === "launching"
                                             || host.phase === "connecting"

                Layout.fillWidth: true

                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, Moonlight host, %2")
                                     .arg(host.label).arg(page.trustText(host.trust))

                contentItem: ColumnLayout {
                    spacing: Tokens.s5

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.BrandGlyph {
                            glyph: "dish-logo"
                            Layout.preferredWidth: Tokens.glyphSm
                            Layout.preferredHeight: Tokens.glyphSm
                            Layout.alignment: Qt.AlignVCenter
                        }
                        ColumnLayout {
                            spacing: Tokens.s0
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter

                            Label {
                                text: host.label
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textBase
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            // The kind, in words: a glyph alone would not say it.
                            Label {
                                text: qsTr("Moonlight host (Sunshine/Apollo)")
                                color: Theme.muted
                                font.pixelSize: Tokens.textMeta
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                        Kit.LiveStat {
                            text: host.modelData.address
                            elide: Text.ElideRight
                            Layout.alignment: Qt.AlignVCenter
                        }
                        // Trust, not liveness. Three words, and never a dot.
                        Kit.CapabilityChip {
                            text: page.trustText(host.trust)
                            tone: page.trustTone(host.trust)
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Kit.CapabilityChip {
                            visible: host.controllers > 0
                            text: qsTr("In use by %1").arg(page.controllerPhrase(host.controllers))
                            tone: Kit.CapabilityChip.Present
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Kit.CapabilityChip {
                            visible: host.phase !== "idle" && host.phase !== "paired"
                            text: page.phaseText(host.phase)
                            tone: page.phaseTone(host.phase)
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    // What the session is running, once there is one. Read-only:
                    // the app belongs to whoever created the session, and the
                    // binding flow is where a new one is chosen.
                    RowLayout {
                        visible: host.sessionUp && host.modelData.appName.length > 0
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.Eyebrow { mutedTone: true; text: qsTr("Session") }
                        Label {
                            text: host.modelData.appName
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textMeta
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Item { Layout.fillWidth: true }

                        Kit.DishButton {
                            visible: host.trust !== "paired"
                            text: host.trust === "remembered" ? qsTr("Pair again") : qsTr("Pair…")
                            variant: Kit.DishButton.Primary
                            enabled: !host.busy && !App.moonlightPairingActive
                            onClicked: pairSheet.openFor(host.hostId, host.label)
                        }
                        // No Connect. Binding a controller starts or joins the
                        // session; unbinding the last one ends it.
                        Kit.DishButton {
                            text: "⋯"
                            variant: Kit.DishButton.Outline
                            Accessible.name: qsTr("More actions for %1").arg(host.label)
                            onClicked: {
                                page.currentHostId = host.hostId;
                                page.currentLabel = host.label;
                                page.currentHasSession = host.sessionUp;
                                hostMenu.popup();
                            }
                        }
                    }
                }
            }
        }

        Label {
            visible: page.hostRows.length > 0
            text: qsTr("Pairing is one-time trust, not a connection. Dish re-checks it when you use a controller.")
            color: Theme.mutedStrong
            font.pixelSize: Tokens.textMeta
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s2
        }
    }

    // ---- Host overflow ------------------------------------------------------
    Menu {
        id: hostMenu

        background: Rectangle {
            // A Menu takes its width from its BACKGROUND, not from its items:
            // the style's default background carries implicitWidth 200, and
            // replacing it with a bare Rectangle drops that to 0.
            implicitWidth: Math.max(Tokens.menuMinWidth,
                                    Math.max(forgetItem.implicitWidth, quitItem.implicitWidth)
                                    + hostMenu.leftPadding + hostMenu.rightPadding)
            color: Theme.surface
            border.width: 1
            border.color: Theme.outline
            radius: Tokens.radiusButton
        }

        MenuItem {
            id: quitItem
            text: qsTr("Quit session")
            enabled: page.currentHasSession

            contentItem: Text {
                text: quitItem.text
                font.pixelSize: Tokens.textSummary
                color: quitItem.enabled ? Theme.onSurface : Theme.disabledFg
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: quitItem.highlighted ? Theme.primaryHover : "transparent"
                radius: Tokens.radiusChip
            }
            onTriggered: App.quitMoonlightApp(page.currentHostId)
        }

        MenuItem {
            id: forgetItem
            text: qsTr("Forget")

            contentItem: Text {
                text: forgetItem.text
                font.pixelSize: Tokens.textSummary
                color: Theme.error
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: forgetItem.highlighted ? Theme.primaryHover : "transparent"
                radius: Tokens.radiusChip
            }
            onTriggered: forgetConfirm.open()
        }
    }

    Kit.ConfirmDialog {
        id: forgetConfirm
        eyebrow: qsTr("Forget")
        heading: qsTr("Forget %1?").arg(page.currentLabel)
        bodyText: qsTr("Its pairing is deleted. You will need the PIN again.")
        acceptText: qsTr("Forget")
        rejectText: qsTr("Cancel")
        destructiveAccept: true
        onAccepted: {
            App.forgetMoonlight(page.currentHostId);
            forgetConfirm.close();
        }
    }

    // ---- Add by address -----------------------------------------------------
    // The discovery fallback: mDNS does not cross every subnet, so a host can
    // always be reached by typing where it lives.
    Kit.ContentDialog {
        id: addSheet
        eyebrow: qsTr("Moonlight host")
        heading: qsTr("Add a host by address")
        acceptText: qsTr("Add")
        rejectText: qsTr("Cancel")
        acceptEnabled: addressField.text.trim().length > 0

        body: [
            Label {
                text: qsTr("Enter the host IP address or hostname. Dish uses the standard Moonlight ports.")
                color: Theme.muted
                font.pixelSize: Tokens.textSummary
                lineHeight: 1.5
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            },
            Kit.KitTextField {
                id: addressField
                placeholderText: qsTr("192.168.1.20")
                Layout.fillWidth: true
            },
            Kit.KitTextField {
                id: nameField
                placeholderText: qsTr("Name (optional)")
                Layout.fillWidth: true
            }
        ]

        onAccepted: {
            App.addMoonlightHost(addressField.text.trim(), nameField.text.trim());
            addressField.clear();
            nameField.clear();
            addSheet.close();
        }
        onClosed: { addressField.clear(); nameField.clear(); }
    }

    // ---- Pairing ------------------------------------------------------------
    // The mirror of the satellite sheet: the PIN is generated HERE and typed
    // into the host's own page, so this sheet DISPLAYS a code rather than
    // asking for one. The code itself is minted in C++ — a PIN is security
    // relevant, and Math.random() is not a suitable source for one.
    Kit.ContentDialog {
        id: pairSheet

        property string hostId: ""
        property string hostName: ""
        property bool rejected: false

        readonly property string pin: App.moonlightPairingPin

        eyebrow: qsTr("Pairing")
        heading: qsTr("Pair with %1").arg(pairSheet.hostName)
        rejectText: qsTr("Cancel")
        acceptText: qsTr("Done")
        acceptEnabled: false

        function openFor(id, name) {
            pairSheet.hostId = id;
            pairSheet.hostName = name;
            pairSheet.rejected = false;
            App.pairMoonlight(id);
            pairSheet.open();
        }

        onRejected: App.cancelMoonlightPairing()

        body: [
            Label {
                text: qsTr("Type %1 into the Moonlight or Sunshine page on %2.")
                          .arg(pairSheet.pin).arg(pairSheet.hostName)
                color: Theme.muted
                font.pixelSize: Tokens.textSummary
                lineHeight: 1.5
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            },
            RowLayout {
                spacing: Tokens.s4
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Tokens.s3
                Layout.bottomMargin: Tokens.s3

                Repeater {
                    model: 4
                    delegate: Rectangle {
                        id: pinCell
                        required property int index

                        implicitWidth: Tokens.s11
                        implicitHeight: Tokens.hitRow
                        radius: Tokens.radiusButton
                        color: Theme.surfaceDim
                        border.width: 1
                        border.color: pairSheet.rejected ? Theme.error : Theme.outline

                        Label {
                            anchors.centerIn: parent
                            text: pinCell.index < pairSheet.pin.length
                                  ? pairSheet.pin.charAt(pinCell.index) : ""
                            color: Theme.primary
                            font.family: Tokens.monoFamily
                            font.pixelSize: Tokens.textHero
                        }
                    }
                }
            },
            RowLayout {
                spacing: Tokens.s5
                Layout.fillWidth: true

                Kit.DishProgressBar {
                    visible: !pairSheet.rejected
                    indeterminate: true
                    Layout.preferredWidth: Tokens.s11 * 2
                }
                Label {
                    text: pairSheet.rejected
                          ? qsTr("Check that the code went into the right host, then try again.")
                          : qsTr("Waiting for the host to accept the PIN…")
                    color: pairSheet.rejected ? Theme.error : Theme.muted
                    font.pixelSize: Tokens.textSummary
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Kit.DishButton {
                    visible: pairSheet.rejected
                    text: qsTr("New code")
                    variant: Kit.DishButton.Outline
                    size: Kit.DishButton.Small
                    onClicked: pairSheet.openFor(pairSheet.hostId, pairSheet.hostName)
                }
            }
        ]
    }

    Connections {
        target: App

        function onMoonlightChanged() {
            if (!pairSheet.visible || pairSheet.hostId.length === 0)
                return;
            // Read the sheet's OWN host: pairing state is global, and a
            // background attempt elsewhere must not dismiss or fail this sheet.
            const session = App.moonlightSession(pairSheet.hostId, "");
            if (session.trust === "paired") {
                pairSheet.rejected = false;
                pairSheet.close();
                // A freshly paired host can be asked what it runs.
                App.refreshMoonlightApps(pairSheet.hostId);
                return;
            }
            pairSheet.rejected = session.state === "pairingRefused";
        }
    }

    // ---- Helpers: tokens to localized copy ----------------------------------
    function trustText(token) {
        switch (token) {
        case "paired":     return qsTr("Paired");
        case "remembered": return qsTr("Remembered");
        default:           return qsTr("Not paired");
        }
    }
    // Amber is the PROBLEM colour, never the working one, so a remembered
    // pairing that simply has not been re-confirmed reads neutral.
    function trustTone(token) {
        switch (token) {
        case "paired":     return Kit.CapabilityChip.Ok;
        case "remembered": return Kit.CapabilityChip.Neutral;
        default:           return Kit.CapabilityChip.Absent;
        }
    }
    function controllerPhrase(n) {
        return qsTr("%n controllers", "", n);
    }
    function phaseText(token) {
        switch (token) {
        case "pairing":    return qsTr("Pairing…");
        case "paired":     return qsTr("Paired");
        case "launching":  return qsTr("Starting…");
        case "connecting": return qsTr("Connecting…");
        case "streaming":  return qsTr("Streaming");
        case "faltering":  return qsTr("Unsteady");
        case "failed":     return qsTr("Failed");
        case "closed":     return qsTr("Disconnected");
        default:           return qsTr("Found");
        }
    }
    function phaseTone(token) {
        switch (token) {
        case "streaming":  return Kit.CapabilityChip.Ok;
        case "paired":     return Kit.CapabilityChip.Present;
        // Working, not wrong: a handshake in flight is not an amber state.
        case "pairing":
        case "launching":
        case "connecting": return Kit.CapabilityChip.Neutral;
        case "faltering":
        case "failed":     return Kit.CapabilityChip.Warn;
        default:           return Kit.CapabilityChip.Neutral;
        }
    }
}
