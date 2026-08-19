// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Connections destination — the inventory of HOSTS: manage the link, report
// the bindings riding on it. There is no path into a binding editor from here:
// a host carries up to four slots, so no row maps to one binding and the
// CARRYING manifest is read-only, never a link.

// Bound: delegates reference the outer `page` id and their `required` model
// props statically.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: page
    title: qsTr("Connections")

    // Rendered by AppShell, not by this body.
    readonly property string headerTitle: qsTr("Connections")
    // Two independent counts can't both be %n in one message, so each half is
    // its own counted message.
    readonly property string headerSub: App.connectionCount === 0
        ? qsTr("%n found · nothing paired yet", "", App.foundCount)
        : qsTr("%n online", "", App.onlineCount) + " · "
          + qsTr("%n paired", "", App.connectionCount)
    readonly property string headerDot: App.connectionCount === 0 ? "muted"
                                        : App.onlineCount > 0 ? "success" : "warning"

    // Where the free Satellite host app comes from (the same link the wizard's
    // destination step offers).
    readonly property string satelliteUrl: "https://dish.tinkernorth.com/downloads/satellite"

    // The host whose overflow menu is open, mirrored as plain values so nothing
    // reads a property off a delegate that may already be recycled.
    property string currentConnectionId: ""
    property string currentLabel: ""

    // Scan on open: entering the destination surfaces reachable satellites
    // without an extra tap; startDiscovery() is guarded manager-side so an
    // in-flight sweep is never double-triggered.
    Component.onCompleted: if (!App.scanning) App.startDiscovery()

    // A satellite can park a pairing request C++-side (a stale-key box we tried
    // to talk to). Open the sheet for it exactly once per parked target.
    Connections {
        target: App
        function onStateChanged() {
            if (App.pairingActive && !pairDialog.visible) {
                // Capture both BEFORE clearing — the clear resets the parked
                // target and these properties with it.
                const name = App.pairingServerName;
                const id = App.pairingServerId;
                App.clearPairingTarget();
                pairDialog.openFor(id, name);
            }
        }
    }

    ColumnLayout {
        id: bodyColumn
        width: parent.width
        spacing: Tokens.s5

        // ---- FOUND ----------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { glyph: "satellite"; label: qsTr("Found") }
            Item { Layout.fillWidth: true }
            Kit.LiveStat {
                live: App.scanning
                text: App.scanning ? qsTr("scanning…")
                                   : qsTr("%n found", "", App.foundCount)
            }
            // ONE control. Discovery here is a bounded ~4 s sweep with no
            // cancel seam, so there is no scan to stop — the verb reports the
            // sweep and the bar below is the busy cue.
            Kit.DishButton {
                text: App.scanning ? qsTr("Scanning…") : qsTr("Scan")
                variant: Kit.DishButton.Outline
                enabled: !App.scanning
                onClicked: App.startDiscovery()
            }
        }

        Kit.DishProgressBar {
            visible: App.scanning
            indeterminate: true
            Layout.fillWidth: true
        }

        // Empty FOUND is a real state in BOTH cases — the copy never claims to
        // be searching after the sweep has finished.
        Kit.EmptyState {
            visible: App.foundCount === 0 && App.scanning
            glyph: "satellite-broadcasting"
            title: qsTr("Looking for satellites")
            body: qsTr("Scanning your network over mDNS and UDP broadcast. Hosts appear here as they answer.")
            showAction: false
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s5
            Layout.bottomMargin: Tokens.s5
        }

        Kit.EmptyState {
            visible: App.foundCount === 0 && !App.scanning
            glyph: "satellite-off"
            title: qsTr("No satellites found")
            body: qsTr("A PC shows up here once the free Satellite app is running on it and both machines are on the same network.")
            showAction: false
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s5
            Layout.bottomMargin: Tokens.s5

            // Two next steps, so the state is a way forward rather than a
            // dead end. EmptyState offers one action slot; the pair rides its
            // column as a row.
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Tokens.s3
                spacing: Tokens.s4

                Kit.DishButton {
                    text: qsTr("Get Satellite ↗")
                    variant: Kit.DishButton.Outline
                    onClicked: App.openExternalUrl(page.satelliteUrl)
                }
                Kit.DishButton {
                    text: qsTr("Scan again")
                    variant: Kit.DishButton.Outline
                    enabled: !App.scanning
                    onClicked: App.startDiscovery()
                }
            }
        }

        // Found rows: seen in the sweep, not remembered. The one-spot rule keeps
        // an id that already has a REMEMBERED row out of this list entirely.
        Repeater {
            model: App.discoveredServers

            delegate: Kit.Card {
                id: foundRow
                required property var modelData
                readonly property string displayName:
                    modelData.name.length > 0 ? modelData.name : modelData.ip

                filled: false
                dense: true
                Layout.fillWidth: true

                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, found, %2")
                                     .arg(foundRow.displayName).arg(foundRow.modelData.ip)

                contentItem: RowLayout {
                    spacing: Tokens.s4

                    Kit.BrandGlyph {
                        glyph: "satellite"
                        Layout.preferredWidth: Tokens.glyphSm
                        Layout.preferredHeight: Tokens.glyphSm
                        Layout.alignment: Qt.AlignVCenter
                    }
                    // Dot and chip always travel together: hue alone is not a
                    // status.
                    Kit.StatusDot {
                        token: "muted"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Label {
                        text: foundRow.displayName
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textBase
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Kit.LiveStat {
                        text: foundRow.modelData.ip + " · " + foundRow.modelData.source
                        elide: Text.ElideRight
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Kit.CapabilityChip {
                        text: qsTr("Found")
                        tone: Kit.CapabilityChip.Neutral
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Kit.DishButton {
                        text: qsTr("Pair…")
                        variant: Kit.DishButton.Primary
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: pairDialog.openFor(foundRow.modelData.id,
                                                      foundRow.displayName)
                    }
                }
            }
        }

        // ---- REMEMBERED -----------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s5
            spacing: Tokens.s4

            Kit.SectionHeader { glyph: "satellite"; label: qsTr("Paired") }
            Item { Layout.fillWidth: true }
            Kit.LiveStat { text: qsTr("%n paired", "", App.connectionCount) }
        }

        Kit.EmptyState {
            visible: App.connectionCount === 0
            glyph: "satellite-off"
            title: qsTr("Nothing paired yet")
            body: qsTr("Pair a satellite and Dish saves it here for next time.")
            showAction: false
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s5
        }

        // One card per remembered host. Content-sized and inert: the page's
        // single scroller owns all overflow, and the model's scoped dataChanged
        // keeps a ~1 Hz latency tick from resetting the view.
        ListView {
            id: hostList
            visible: App.connectionCount > 0
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            interactive: false
            spacing: Tokens.s5
            model: App.connectionModel

            delegate: Kit.Card {
                id: host
                width: ListView.view ? ListView.view.width : implicitWidth

                // ConnectionListModel roles, see docs/QML_CONTRACT.md.
                required property string connectionId
                required property string label
                required property string ip
                required property int udpPort
                required property string linkState
                required property string chip
                required property string dotColor
                required property string glyph
                required property bool liveLink
                required property string latencyText
                required property int latencySamples

                readonly property bool needsPairing: chip === "needsPairing"
                readonly property bool connecting: linkState === "connecting"
                // Latency renders only while the link is genuinely live and has
                // samples — never "~0 ms".
                readonly property bool showLatency:
                    latencySamples > 0 && (linkState === "connected" || chip === "unstable")
                readonly property var carrying:
                    page.carryingLines(connectionId, App.boundSlotCount)

                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, %2").arg(host.label).arg(page.chipText(host.chip))
                                 + (host.showLatency ? " · " + host.latencyText : "")

                contentItem: ColumnLayout {
                    spacing: Tokens.s5

                    // ── Header row: glyph · dot · name · address · chip ──────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.BrandGlyph {
                            id: hostGlyph
                            glyph: hostGlyph.glyphForToken(host.glyph)
                            Layout.preferredWidth: Tokens.glyphSm
                            Layout.preferredHeight: Tokens.glyphSm
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Kit.StatusDot {
                            token: host.dotColor
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Label {
                            text: host.label
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                        }
                        // The ONE "•" in the app: the contract-defined detail
                        // composition. Everything else separates with "·".
                        Kit.LiveStat {
                            text: qsTr("%1 • UDP %2").arg(host.ip).arg(host.udpPort)
                            elide: Text.ElideRight
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Kit.LiveStat {
                            visible: host.showLatency
                            live: true
                            text: qsTr("%1 · last %2 pings")
                                      .arg(host.latencyText).arg(host.latencySamples)
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Kit.CapabilityChip {
                            text: page.chipText(host.chip)
                            tone: page.chipTone(host.chip)
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    // ── CARRYING: the read-only manifest, never a link ───────
                    Kit.Card {
                        filled: false
                        dense: true
                        Layout.fillWidth: true

                        // Announced as one block: a screen reader should hear
                        // what the host carries, not walk six unlabelled lines.
                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Carrying") + ": "
                                         + (host.carrying.length > 0
                                            ? host.carrying.join(", ")
                                            : qsTr("Nothing bound yet"))

                        contentItem: ColumnLayout {
                            spacing: Tokens.s2

                            Kit.Eyebrow {
                                mutedTone: true
                                text: qsTr("Carrying")
                            }
                            Label {
                                visible: host.carrying.length === 0
                                text: qsTr("Nothing bound yet")
                                color: Theme.mutedStrong
                                font.pixelSize: Tokens.textMeta
                            }
                            Repeater {
                                model: host.carrying
                                delegate: Label {
                                    id: manifestLine
                                    required property int index
                                    required property string modelData
                                    text: manifestLine.modelData
                                    color: manifestLine.index === 0 ? Theme.onSurface
                                                                    : Theme.mutedStrong
                                    font.pixelSize: Tokens.textMeta
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }

                    // ── Action row: one primary inline, Forget behind ⋯ ──────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Item { Layout.fillWidth: true }

                        Kit.DishButton {
                            visible: host.liveLink
                            text: qsTr("Disconnect")
                            variant: Kit.DishButton.Outline
                            onClicked: App.disconnectConnection(host.connectionId)
                        }
                        // Terminal until re-paired: the key was rejected, so
                        // Reconnect would only fail again.
                        Kit.DishButton {
                            visible: !host.liveLink && host.needsPairing
                            text: qsTr("Pair…")
                            variant: Kit.DishButton.Primary
                            onClicked: pairDialog.openFor(host.connectionId, host.label)
                        }
                        Kit.DishButton {
                            visible: !host.liveLink && !host.needsPairing
                            text: host.connecting ? qsTr("Connecting…") : qsTr("Connect")
                            variant: Kit.DishButton.Outline
                            enabled: !host.connecting
                            onClicked: App.reconnectConnection(host.connectionId)
                        }
                        // The destructive action sits behind the overflow so it
                        // is never the mis-click next to Disconnect.
                        Kit.DishButton {
                            text: "⋯"
                            variant: Kit.DishButton.Outline
                            Accessible.name: qsTr("More actions for %1").arg(host.label)
                            onClicked: {
                                page.currentConnectionId = host.connectionId;
                                page.currentLabel = host.label;
                                hostMenu.popup();
                            }
                        }
                    }
                }
            }
        }

        // ---- Non-visual join ------------------------------------------------
        // The manifest needs the pads riding each host, which no connection role
        // carries. This mirrors the slot model the Home page already renders
        // rather than adding an App member.
        Item {
            visible: false

            Repeater {
                id: slotJoin
                model: App.slotModel
                delegate: Item {
                    required property string name
                    required property string boundConnectionId
                    required property string emulateName
                }
            }
        }
    }

    // ---- Host overflow menu -------------------------------------------------
    Menu {
        id: hostMenu

        background: Rectangle {
            color: Theme.surface
            border.width: 1
            border.color: Theme.outline
            radius: Tokens.radiusButton
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
            onTriggered: page.confirmForget()
        }
    }

    // ---- Forget confirm -----------------------------------------------------
    // It names every pad it will drop; the pairing key goes with it.
    Kit.ConfirmDialog {
        id: forgetConfirm

        property var pads: []

        eyebrow: qsTr("Forget")
        heading: qsTr("Forget %1?").arg(page.currentLabel)
        bodyText: qsTr("Its pairing key is deleted — you’ll need the PIN again.")
                  + (forgetConfirm.pads.length > 0
                     ? "\n" + page.bindingsDroppedText(forgetConfirm.pads.length) : "")
        bulletLines: forgetConfirm.pads
        acceptText: qsTr("Forget")
        rejectText: qsTr("Cancel")
        destructiveAccept: true

        onAccepted: {
            App.forgetConnection(page.currentConnectionId);
            forgetConfirm.close();
        }
    }

    // ---- Telemetry footer ---------------------------------------------------
    footer: Item {
        implicitHeight: telemetryRow.implicitHeight + Tokens.s8

        RowLayout {
            id: telemetryRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Tokens.pagePadding
            anchors.rightMargin: Tokens.pagePadding
            spacing: Tokens.s9

            Kit.LiveStat {
                text: qsTr("%n online", "", App.onlineCount) + " · "
                      + qsTr("%n bindings", "", App.boundSlotCount)
            }
            Item { Layout.fillWidth: true }
            Kit.LiveStat {
                text: qsTr("%n paired", "", App.connectionCount)
            }
        }
    }

    // ---- PAIRING: the shared both-paths-at-once sheet ------------------------
    // Path A (type the satellite's 6-digit PIN) is always live; Path B (our
    // 4-digit clientPin) is sent automatically by openFor(). Same-module bare
    // type — the wizard's destination step opens the same sheet.
    PairingDialog { id: pairDialog }

    // ---- Helpers (presentation only; wording owned by this page) ------------

    function confirmForget() {
        if (page.currentConnectionId.length === 0)
            return;
        forgetConfirm.pads = page.carryingPads(page.currentConnectionId);
        forgetConfirm.open();
    }

    // "2 bindings ride on it and will be dropped:". %n so the verb agrees in
    // languages that inflect it — English alternates rides/ride, and Bosnian
    // needs a third form again at 2-4.
    function bindingsDroppedText(n) {
        return qsTr("%n bindings ride on it and will be dropped:", "", n);
    }

    // The pads riding `connectionId`, by name — what a Forget will drop.
    function carryingPads(connectionId) {
        var out = [];
        if (connectionId.length === 0)
            return out;
        for (var i = 0; i < slotJoin.count; ++i) {
            var entry = slotJoin.itemAt(i);
            if (entry && entry.boundConnectionId === connectionId)
                out.push(entry.name);
        }
        return out;
    }

    // The manifest lines: "<pad> — as <type>". The type is a property of the
    // binding, so it prints here and never on the pad. `epoch` is read by the
    // caller's binding so the join re-runs when the slot list moves (the mirror
    // carries no NOTIFY of its own).
    function carryingLines(connectionId, epoch) {
        var out = [];
        if (connectionId.length === 0)
            return out;
        for (var i = 0; i < slotJoin.count; ++i) {
            var entry = slotJoin.itemAt(i);
            if (!entry || entry.boundConnectionId !== connectionId)
                continue;
            out.push(entry.emulateName.length > 0
                     ? qsTr("%1 — as %2").arg(entry.name).arg(entry.emulateName)
                     : entry.name);
        }
        return out;
    }

    // Localized chip text/tone for a link-state token — the same ladders Home
    // renders from the identical tokens. The C++ vends the token, not the copy.
    function chipText(token) {
        switch (token) {
        case "found":        return qsTr("Found");
        case "needsPairing": return qsTr("Needs pairing");
        case "offline":      return qsTr("Offline");
        case "ready":        return qsTr("Ready");
        case "connecting":   return qsTr("Connecting…");
        case "online":       return qsTr("Online");
        case "unstable":     return qsTr("Unsteady");
        default:             return token;
        }
    }
    function chipTone(token) {
        switch (token) {
        case "online":       return Kit.CapabilityChip.Ok;
        case "connecting":   return Kit.CapabilityChip.Warn;
        case "unstable":     return Kit.CapabilityChip.Warn;
        case "needsPairing": return Kit.CapabilityChip.Warn;
        case "ready":        return Kit.CapabilityChip.Present;
        default:             return Kit.CapabilityChip.Neutral;
        }
    }
}
