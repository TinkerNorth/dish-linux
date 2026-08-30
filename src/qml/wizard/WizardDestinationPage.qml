// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wizard page 2 — Destination. Which PC this pad should drive, pairing it if
// need be; the live scan IS the step, not a dialog over it. The trap here:
// pairingSucceeded is a GLOBAL signal — a background reconnect fires it too — so
// the pending host id is stored and compared before the wizard may move.

// Bound: the row delegates read the outer `page` id alongside their required
// model properties.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit
import "../pages" as Pages

ColumnLayout {
    id: page

    property BindingDraft draft

    // The wizard advances only when the pairing THIS page started lands.
    signal advanceRequested()

    // ── The wizard's step contract ──────────────────────────────────────────
    readonly property bool canAdvance: page.draft.hasDestination
    readonly property string primaryLabel: page.selectedNeedsPairing ? qsTr("Pair…")
                                                                     : qsTr("Continue ›")
    readonly property string hint: page.hostCount > 0
                                   ? qsTr("Picking an unpaired host asks for its PIN.")
                                   : qsTr("The scan keeps running while you read.")
    readonly property bool modalOpen: pairSheet.visible

    function primaryActivated() {
        if (!page.selectedNeedsPairing)
            return true;
        // The `…` means a dialog opens; the wizard does not step.
        page.pendingHostId = page.draft.hostId;
        pairSheet.openFor(page.draft.hostId, page.draft.hostName);
        return false;
    }

    function activated() {
        if (!App.scanning)
            App.startDiscovery();
        // The Moonlight sweep is a separate one-shot, and trust is re-asked on
        // entering rather than watched: a Moonlight host reports no liveness.
        if (!App.moonlightScanning)
            App.scanMoonlight();
        for (let i = 0; i < page.moonlightRows.length; ++i)
            App.probeMoonlightHost(page.moonlightRows[i].uuid);
    }

    // ── Page state ──────────────────────────────────────────────────────────
    readonly property var moonlightRows: App.moonlightHosts
    readonly property int hostCount: App.connectionModel.count + App.discoveredServers.length
                                     + page.moonlightRows.length
    property bool selectedNeedsPairing: false
    // The host THIS page asked the user to pair. Compared on pairingSucceeded.
    property string pendingHostId: ""
    // Bumped on every state move so the invokable-backed slot accounting in the
    // row sub-lines re-evaluates.
    property int accounting: 0

    readonly property string satelliteUrl: "https://dish.tinkernorth.com/downloads/satellite"

    spacing: Tokens.s6

    // ── Row copy ────────────────────────────────────────────────────────────

    function chipTextFor(chip) {
        switch (chip) {
        case "found":
            return qsTr("Found");
        case "needsPairing":
            return qsTr("Needs pairing");
        case "offline":
            return qsTr("Offline");
        case "ready":
            return qsTr("Ready");
        case "connecting":
            return qsTr("Connecting…");
        case "online":
            return qsTr("Online");
        case "unstable":
            return qsTr("Unsteady");
        }
        return "";
    }

    function chipToneFor(chip) {
        if (chip === "online")
            return Kit.CapabilityChip.Ok;
        if (chip === "needsPairing" || chip === "connecting" || chip === "unstable")
            return Kit.CapabilityChip.Warn;
        if (chip === "ready")
            return Kit.CapabilityChip.Present;
        return Kit.CapabilityChip.Neutral;
    }

    // Never assert a slot NUMBER before bindSlot allocates one. The zero case is
    // its own message: it states a consequence rather than counting anything.
    function slotsFreeText(connectionId) {
        const free = page.accounting >= 0
                   ? App.hostSlotCapacity() - App.hostBoundSlotCount(connectionId) : 0;
        if (free <= 0)
            return qsTr("0 slots free — one pad will be unbound");
        return qsTr("%n slots free", "", free);
    }

    function rowSubText(ip, source, connectionId) {
        const parts = [];
        if (ip.length > 0)
            parts.push(ip);
        if (source.length > 0)
            parts.push(source);
        parts.push(page.slotsFreeText(connectionId));
        return parts.join(" · ");
    }

    function pick(id, name, needsPairing) {
        page.selectedNeedsPairing = needsPairing;
        page.draft.chooseDestination(id, name, "satellite");
    }

    // A Moonlight host is picked whatever its trust: pairing is remembered
    // trust verified lazily, so an unpaired host is a state the session step
    // renders and offers to fix, never a reason to refuse the destination.
    function pickMoonlight(id, name) {
        page.selectedNeedsPairing = false;
        page.pendingHostId = "";
        page.draft.chooseDestination(id, name, "moonlight");
    }

    function moonlightSubText(row) {
        const parts = [];
        if (row.address.length > 0)
            parts.push(row.address);
        // A Moonlight session refuses a fifth pad rather than pushing one off,
        // so the zero case says what actually happens.
        const free = App.hostSlotCapacity() - App.hostBoundSlotCount(row.uuid);
        parts.push(free > 0 ? qsTr("%n slots free", "", free) : qsTr("full"));
        return parts.join(" · ");
    }

    function trustText(token) {
        switch (token) {
        case "paired":     return qsTr("Paired");
        case "remembered": return qsTr("Remembered");
        default:           return qsTr("Not paired");
        }
    }
    function trustTone(token) {
        switch (token) {
        case "paired":     return Kit.CapabilityChip.Ok;
        case "remembered": return Kit.CapabilityChip.Neutral;
        default:           return Kit.CapabilityChip.Absent;
        }
    }

    Connections {
        target: App

        function onStateChanged() { page.accounting += 1; }

        // Gate on THIS page's pending host: a background reconnect raises the
        // same signal, and following it would bind a host the user never saw.
        function onPairingSucceeded() {
            if (page.pendingHostId.length === 0)
                return;
            if (page.pendingHostId !== page.draft.hostId)
                return;
            page.pendingHostId = "";
            page.selectedNeedsPairing = false;
            page.advanceRequested();
        }
    }

    // ── Head ────────────────────────────────────────────────────────────────
    Label {
        text: qsTr("Which PC?")
        color: Theme.onSurface
        font.pixelSize: Tokens.textStatus
        font.bold: true
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        text: page.hostCount > 0 ? qsTr("Pick the PC this controller should drive.")
                                 : qsTr("Scanning your Wi-Fi for a Satellite host.")
        color: Theme.muted
        font.pixelSize: Tokens.textSummary
        lineHeight: 1.5
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    Kit.DishProgressBar {
        visible: App.scanning
        indeterminate: true
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2
    }

    // ── The list ────────────────────────────────────────────────────────────
    RowLayout {
        spacing: Tokens.s5
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2

        Kit.Eyebrow {
            mutedTone: true
            text: qsTr("Scanning your Wi-Fi")
            Layout.fillWidth: true
        }
        // Counts the rows the picker OFFERS (remembered ∪ found) — App.foundCount
        // is the un-remembered subset and would read 0 beside a listed host.
        Label {
            text: qsTr("%n found", "", page.hostCount)
            color: Theme.mutedStrong
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textChip

            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // Remembered satellites — model-backed, so a latency tick patches a row
    // instead of resetting the picker.
    ListView {
        id: rememberedList
        visible: App.connectionModel.count > 0
        interactive: false
        spacing: Tokens.s3
        model: App.connectionModel
        Layout.fillWidth: true
        // The layout owns the height: a content-sized, non-interactive list so
        // the page's own scroller is the only scroll region.
        Layout.preferredHeight: contentHeight

        delegate: Kit.SelectRow {
            id: rememberedRow

            required property string connectionId
            required property string label
            required property string ip
            required property string chip
            required property string dotColor

            readonly property bool needsPin: rememberedRow.chip === "needsPairing"
                                             || rememberedRow.chip === "found"

            width: rememberedList.width
            selected: page.draft.hostId === rememberedRow.connectionId
            title: rememberedRow.label
            subtitle: page.rowSubText(rememberedRow.ip, "", rememberedRow.connectionId)
            dotToken: rememberedRow.dotColor
            chipText: page.chipTextFor(rememberedRow.chip)
            chipTone: page.chipToneFor(rememberedRow.chip)

            onPicked: page.pick(rememberedRow.connectionId, rememberedRow.label,
                                rememberedRow.needsPin)
        }
    }

    // The un-remembered rest of the scan. Every one of these owes a PIN.
    Repeater {
        model: App.discoveredServers

        delegate: Kit.SelectRow {
            id: foundRow

            required property var modelData

            Layout.fillWidth: true
            selected: page.draft.hostId === foundRow.modelData.id
            title: foundRow.modelData.name
            subtitle: page.rowSubText(foundRow.modelData.ip, foundRow.modelData.source,
                                      foundRow.modelData.id)
            dotToken: "warning"
            chipText: qsTr("Needs pairing, PIN")
            chipTone: Kit.CapabilityChip.Warn

            onPicked: page.pick(foundRow.modelData.id, foundRow.modelData.name, true)
        }
    }

    // ── Moonlight hosts ─────────────────────────────────────────────────────
    // Its own section, not more rows above: the two host kinds pair
    // differently, and one merged column would make the trust word and the
    // Pair verb each mean two things.
    RowLayout {
        visible: page.moonlightRows.length > 0
        spacing: Tokens.s5
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s5

        Kit.Eyebrow {
            mutedTone: true
            text: qsTr("Moonlight hosts")
            Layout.fillWidth: true
        }
        Label {
            text: qsTr("%n found", "", page.moonlightRows.length)
            color: Theme.mutedStrong
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textChip

            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    Repeater {
        model: page.moonlightRows

        delegate: Kit.SelectRow {
            id: moonRow

            required property var modelData

            Layout.fillWidth: true
            selected: page.draft.hostId === moonRow.modelData.uuid
            title: moonRow.modelData.name
            subtitle: qsTr("Moonlight host · %1").arg(page.moonlightSubText(moonRow.modelData))
            chipText: page.trustText(moonRow.modelData.trust)
            chipTone: page.trustTone(moonRow.modelData.trust)

            onPicked: page.pickMoonlight(moonRow.modelData.uuid, moonRow.modelData.name)
        }
    }

    Label {
        visible: page.moonlightRows.length === 0 && page.hostCount > 0
        text: qsTr("A PC appears here once Sunshine, Apollo or Wolf is running on it and both machines are on the same network. You can also add one by address.")
        color: Theme.mutedStrong
        font.pixelSize: Tokens.textMeta
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2
    }

    // Empty is a STATE, with the next step in it — never a bare spinner and
    // never a bare "none found".
    Kit.EmptyState {
        visible: page.hostCount === 0
        glyph: "satellite-off"
        title: qsTr("No PCs found yet")
        body: qsTr("A PC shows up here once the free Satellite app is running on it, or once Sunshine, Apollo or Wolf is, and both machines are on the same network.")
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s8
    }

    Item {
        Layout.fillHeight: true
        Layout.minimumHeight: Tokens.s5
    }

    // ── Bottom-pinned: the two things that actually fix an empty scan ───────
    Kit.Callout {
        visible: page.hostCount === 0
        tone: Kit.Callout.Info
        text: qsTr("Don’t see your PC? Install the free Satellite app on it.")
        Layout.fillWidth: true

        Kit.DishButton {
            text: qsTr("Get Satellite ↗")
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            onClicked: App.openExternalUrl(page.satelliteUrl)
        }
        Kit.DishButton {
            text: App.scanning ? qsTr("Scanning…") : qsTr("Rescan")
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            enabled: !App.scanning
            onClicked: App.startDiscovery()
        }
    }

    Label {
        visible: page.hostCount > 0
        text: qsTr("Pairing is one-time. After this, Dish reconnects to it by itself.")
        color: Theme.mutedStrong
        font.pixelSize: Tokens.textMeta
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // The one shared sheet, two callers (here and Connections).
    Pages.PairingDialog { id: pairSheet }
}
