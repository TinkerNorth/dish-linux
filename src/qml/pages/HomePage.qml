// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Home destination — the signal-path wiring diagram: one BOXED row per bound
// slot (pad → wire → satellite, the binding printed underneath), a ghost row per
// dangling pad. Two rules: the emulated type belongs to the BINDING, so it prints
// in the strip and never on the pad sub-line; and a dot always travels with a
// chip, both read from the model's tokens rather than a literal.

// Bound: delegates reference the outer `page` id and their `required` model props
// statically.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: page
    title: qsTr("Home")

    // ---- Shell header contract (rendered by AppShell) -----------------------
    readonly property string headerTitle: qsTr("Home")
    readonly property string headerSub: {
        if (App.slotCount === 0 && App.connectionCount === 0)
            return qsTr("Plug in a controller and pair a Satellite to get started");
        if (App.streamingSlotCount > 0)
            return page.controllersStreamingText(App.streamingSlotCount)
                   + " · " + page.satellitesOnlineText(App.onlineCount);
        if (App.onlineCount > 0)
            return page.satellitesOnlineText(App.onlineCount) + qsTr(" · nothing streaming");
        return qsTr("Nothing streaming");
    }
    readonly property string headerDot: {
        if (App.streamingSlotCount > 0)
            return "success";
        if (App.onlineCount > 0)
            return "primary";
        return "muted";
    }
    // The ONE streaming pill in the app is the SHELL's, on every page; this page
    // deliberately declares none of its own.

    // var-typed so the shell's dynamic `shellApi` resolves without lint warnings.
    readonly property var shellStack: StackView.view
    readonly property var shellApi: shellStack ? shellStack.shellApi : null

    // Signal-path geometry. The nodes flex between these bounds and elide with a
    // full-name tooltip; below the stack breakpoint the row goes vertical rather
    // than squeezing (a DPI-scaled window cannot hold three fixed columns).
    readonly property int nodeMinWidth: 220
    readonly property int nodePreferredWidth: 250
    readonly property int nodeMaxWidth: 340
    readonly property int wireMinWidth: 120

    // Raw axis full ranges the dead-zone percentages map over (the same pair the
    // Dead zones page presents its sliders on).
    readonly property real stickRange: 32767
    readonly property real triggerRange: 255

    readonly property bool isEmpty: App.slotCount === 0
    readonly property bool stacked: bodyColumn.width > 0
                                    && bodyColumn.width < Tokens.stackBreakpoint

    // ---- Row keyboard model -------------------------------------------------
    // The list is one focus stop. The current row's facts are mirrored here as
    // plain values so nothing reads a property off a recycled delegate.
    property string currentSlotId: ""
    property string currentSlotName: ""
    property bool currentBound: false
    property bool currentRemappable: false

    // Per-device dead-zone rows. The slot id IS the SDL bridge device id, so the
    // strip's dead-zone chip joins on it; re-pulled when the store moves.
    property var deadzoneRows: App.deadzoneDevices()

    Connections {
        target: App
        function onDeadzonesChanged() { page.deadzoneRows = App.deadzoneDevices(); }
    }

    ColumnLayout {
        id: bodyColumn
        width: parent.width
        spacing: Tokens.s6

        // ---- Column eyebrows: Pad · This PC · Satellite ---------------------
        // Only while the three columns actually line up.
        RowLayout {
            visible: !page.isEmpty && !page.stacked
            Layout.fillWidth: true
            spacing: Tokens.s7

            Kit.Eyebrow {
                mutedTone: true
                // Disambiguated: bare "Pad" is also the touchpad routing mode
                // (Off · Pad · Mouse) and one message cannot carry both senses —
                // French renders that one "Pavé", a touchpad, not a controller.
                text: qsTr("Pad", "the controller column of the wire diagram")
                Layout.preferredWidth: page.nodePreferredWidth
            }
            Kit.Eyebrow {
                mutedTone: true
                text: qsTr("This PC")
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
                Layout.minimumWidth: page.wireMinWidth
            }
            Kit.Eyebrow {
                mutedTone: true
                text: qsTr("Satellite")
                Layout.preferredWidth: page.nodePreferredWidth
            }
        }

        // ---- The empty promise: the shape the wizard is going to fill in ----
        Kit.Card {
            visible: page.isEmpty
            filled: false
            dense: true
            Layout.fillWidth: true

            contentItem: GridLayout {
                columns: page.stacked ? 1 : 3
                columnSpacing: Tokens.s7
                rowSpacing: Tokens.s5

                Kit.ActionCard {
                    placeholder: true
                    title: qsTr("Set up a controller")
                    subtitle: qsTr("Plug one in, then pick where it goes")
                    Layout.fillWidth: true
                    Layout.minimumWidth: page.nodeMinWidth
                    Layout.preferredWidth: page.nodePreferredWidth
                    Layout.maximumWidth: page.stacked ? -1 : page.nodeMaxWidth
                }
                Kit.WireLine {
                    live: false
                    label: qsTr("nothing to draw")
                    Layout.fillWidth: true
                    Layout.minimumWidth: page.wireMinWidth
                    Layout.alignment: Qt.AlignVCenter
                }
                Kit.ActionCard {
                    placeholder: true
                    title: qsTr("No destination")
                    subtitle: qsTr("nothing paired")
                    Layout.fillWidth: true
                    Layout.minimumWidth: page.nodeMinWidth
                    Layout.preferredWidth: page.nodePreferredWidth
                    Layout.maximumWidth: page.stacked ? -1 : page.nodeMaxWidth
                }
            }
        }

        Kit.EmptyState {
            visible: page.isEmpty
            glyph: "satellite-off"
            title: qsTr("Nothing set up yet")
            body: qsTr("Setup fills this diagram in, one answer at a time — and hands it back finished.")
            actionText: qsTr("Set up Dish ›")
            showAction: true
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s6
            onActionRequested: if (page.shellApi) page.shellApi.openSetupWizard("")
        }

        // ---- One row per pad ------------------------------------------------
        ListView {
            id: wireList
            visible: !page.isEmpty
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            interactive: false
            spacing: Tokens.s6
            model: App.slotModel
            activeFocusOnTab: true
            keyNavigationEnabled: true

            Keys.onReturnPressed: page.editCurrentRow()
            Keys.onEnterPressed: page.editCurrentRow()
            Keys.onMenuPressed: page.popupRowMenu()
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                    page.popupRowMenu();
                    event.accepted = true;
                }
            }

            delegate: Item {
                id: rowRoot

                // Slot roles this row consumes, plus the sat* join.
                required property int index
                required property string slotId
                required property string name
                required property bool bound
                required property string boundConnectionId
                required property string boundLabel
                required property bool live
                required property bool registering
                required property bool bluetooth
                required property bool remappable
                required property string emulateName
                required property bool hasMotion
                required property bool hasTouchpad
                required property bool hasLightbar
                required property int batteryLevel
                required property int batteryStatus
                required property bool batteryKnown
                required property int gamepadHz
                required property bool gamepadHzLive
                required property bool gamepadHzShown
                required property string pathPhase
                required property string satIp
                required property string satLinkState
                required property string satChip
                required property string satDotColor
                required property string satGlyph
                required property string satLatencyText
                required property int satLatencySamples

                // Latency rides the label only while genuinely measured (the
                // same gate the Connections rows apply) — never "~0 ms".
                readonly property bool showLatency:
                    satLatencySamples > 0 && (satLinkState === "connected"
                                              || satChip === "unstable")
                readonly property bool ghost: !bound && !registering

                width: ListView.view ? ListView.view.width : implicitWidth
                height: rowCard.implicitHeight

                Accessible.role: Accessible.ListItem
                Accessible.name: rowRoot.bound
                                 ? qsTr("%1, bound to %2, %3")
                                       .arg(rowRoot.name).arg(rowRoot.boundLabel)
                                       .arg(page.chipText(rowRoot.satChip))
                                   + (rowRoot.showLatency ? " · " + rowRoot.satLatencyText : "")
                                 : qsTr("%1, not bound").arg(rowRoot.name)

                ListView.onIsCurrentItemChanged: {
                    if (rowRoot.ListView.isCurrentItem)
                        page.setCurrentRow(rowRoot.slotId, rowRoot.name,
                                           rowRoot.bound, rowRoot.remappable);
                }
                Component.onCompleted: {
                    if (rowRoot.ListView.isCurrentItem)
                        page.setCurrentRow(rowRoot.slotId, rowRoot.name,
                                           rowRoot.bound, rowRoot.remappable);
                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        wireList.currentIndex = rowRoot.index;
                        page.setCurrentRow(rowRoot.slotId, rowRoot.name,
                                           rowRoot.bound, rowRoot.remappable);
                        page.popupRowMenu();
                    }
                }

                // The house focus ring is drawn OUTSIDE the box.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -Tokens.s1
                    radius: Tokens.radiusCard + Tokens.s1
                    visible: wireList.activeFocus && rowRoot.ListView.isCurrentItem
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.focusRing
                }

                Kit.Card {
                    id: rowCard
                    anchors.fill: parent
                    dense: true
                    filled: !rowRoot.ghost

                    contentItem: ColumnLayout {
                        spacing: Tokens.s5

                        GridLayout {
                            Layout.fillWidth: true
                            columns: page.stacked ? 1 : 3
                            columnSpacing: Tokens.s7
                            rowSpacing: Tokens.s5

                            // ── Pad node ────────────────────────────────────
                            Kit.Card {
                                filled: false
                                dense: true
                                Layout.fillWidth: true
                                Layout.minimumWidth: page.nodeMinWidth
                                Layout.preferredWidth: page.nodePreferredWidth
                                Layout.maximumWidth: page.stacked ? -1 : page.nodeMaxWidth
                                Layout.alignment: Qt.AlignVCenter

                                // Declared, not attached: the ATTACHED ToolTip
                                // resolves its delegate through QtQuick.Controls,
                                // which this file does not import, so it logs
                                // "Component is not ready" and never appears.
                                Kit.DishToolTip {
                                    id: padTip
                                    text: rowRoot.name
                                    visible: padHover.hovered && padName.truncated
                                    delay: Tokens.durNormal
                                    y: -padTip.implicitHeight - Tokens.s3
                                }

                                HoverHandler { id: padHover }

                                contentItem: ColumnLayout {
                                    spacing: Tokens.s1

                                    Label {
                                        id: padName
                                        text: rowRoot.name
                                        color: rowRoot.ghost ? Theme.mutedStrong : Theme.onSurface
                                        font.pixelSize: Tokens.textBase
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        text: page.padSubText(rowRoot)
                                        color: Theme.mutedStrong
                                        font.pixelSize: Tokens.textMeta
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }
                            }

                            // ── The wire ────────────────────────────────────
                            Kit.WireLine {
                                live: rowRoot.live
                                label: page.wireLabel(rowRoot)
                                Layout.fillWidth: true
                                Layout.minimumWidth: page.wireMinWidth
                                Layout.alignment: Qt.AlignVCenter

                                Kit.DishToolTip {
                                    id: wireTip
                                    text: qsTr("Report rate: measured. Latency: median round-trip ÷ 2 over the last 64 pings.")
                                    visible: wireHover.hovered && rowRoot.live
                                    delay: Tokens.durNormal
                                    y: -wireTip.implicitHeight - Tokens.s3
                                }

                                HoverHandler { id: wireHover }
                            }

                            // ── Satellite node (bound) ──────────────────────
                            Kit.Card {
                                visible: rowRoot.bound
                                filled: false
                                dense: true
                                Layout.fillWidth: true
                                Layout.minimumWidth: page.nodeMinWidth
                                Layout.preferredWidth: page.nodePreferredWidth
                                Layout.maximumWidth: page.stacked ? -1 : page.nodeMaxWidth
                                Layout.alignment: Qt.AlignVCenter

                                Kit.DishToolTip {
                                    id: satTip
                                    text: rowRoot.boundLabel
                                    visible: satHover.hovered && satName.truncated
                                    delay: Tokens.durNormal
                                    y: -satTip.implicitHeight - Tokens.s3
                                }

                                HoverHandler { id: satHover }

                                contentItem: RowLayout {
                                    spacing: Tokens.s4

                                    Kit.BrandGlyph {
                                        id: satGlyphImage
                                        glyph: satGlyphImage.glyphForToken(rowRoot.satGlyph)
                                        Layout.preferredWidth: Tokens.glyphSm
                                        Layout.preferredHeight: Tokens.glyphSm
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    Kit.StatusDot {
                                        token: rowRoot.satDotColor
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: Tokens.s1

                                        Label {
                                            id: satName
                                            text: rowRoot.boundLabel
                                            color: Theme.onSurface
                                            font.pixelSize: Tokens.textBase
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Kit.LiveStat {
                                            text: page.satSubText(rowRoot, App.boundSlotCount)
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                    Kit.CapabilityChip {
                                        text: page.chipText(rowRoot.satChip)
                                        tone: page.chipTone(rowRoot.satChip)
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                }
                            }

                            // ── Satellite cell (dangling pad) ───────────────
                            Kit.ActionCard {
                                visible: rowRoot.ghost
                                title: qsTr("Bind…")
                                subtitle: qsTr("Choose a satellite for this pad")
                                Layout.fillWidth: true
                                Layout.minimumWidth: page.nodeMinWidth
                                Layout.preferredWidth: page.nodePreferredWidth
                                Layout.maximumWidth: page.stacked ? -1 : page.nodeMaxWidth
                                Layout.alignment: Qt.AlignVCenter
                                onClicked: if (page.shellApi)
                                               page.shellApi.openSetupWizard(rowRoot.slotId)
                            }

                            // A registering pad's cell stays empty (nothing to
                            // bind yet); the column holds so the wire geometry
                            // does not jump when it resolves.
                            Item {
                                visible: !rowRoot.bound && rowRoot.registering
                                Layout.fillWidth: true
                                Layout.minimumWidth: page.nodeMinWidth
                                Layout.preferredWidth: page.nodePreferredWidth
                            }
                        }

                        // ── The binding strip, under a hairline ─────────────
                        Rectangle {
                            visible: rowRoot.bound
                            implicitHeight: 1
                            color: Theme.outlineSubtle
                            Layout.fillWidth: true
                        }
                        Kit.BindingStrip {
                            visible: rowRoot.bound
                            chips: page.stripChips(rowRoot, App.boundSlotCount)
                            availableWidth: rowCard.availableWidth
                            Layout.fillWidth: true
                            onEditRequested: page.openBindingEditor(rowRoot.slotId)
                        }
                    }
                }
            }
        }

        // ---- The bare "Set up a controller" invitation row -------------------
        GridLayout {
            visible: !page.isEmpty
            Layout.fillWidth: true
            columns: page.stacked ? 1 : 3
            columnSpacing: Tokens.s7
            rowSpacing: Tokens.s5

            Kit.ActionCard {
                showPlus: true
                title: qsTr("Set up a controller")
                subtitle: qsTr("Plug one in, then pick where it goes")
                Layout.fillWidth: true
                Layout.minimumWidth: page.nodeMinWidth
                Layout.preferredWidth: page.nodePreferredWidth
                Layout.maximumWidth: page.stacked ? -1 : page.nodeMaxWidth
                onClicked: if (page.shellApi) page.shellApi.openSetupWizard("")
            }
            Kit.WireLine {
                live: false
                showGlyph: false
                Layout.fillWidth: true
                Layout.minimumWidth: page.wireMinWidth
                Layout.alignment: Qt.AlignVCenter
            }
            Item {
                visible: !page.stacked
                Layout.fillWidth: true
                Layout.minimumWidth: page.nodeMinWidth
                Layout.preferredWidth: page.nodePreferredWidth
            }
        }

        // ---- Non-visual joins ------------------------------------------------
        // A row needs to know its ordinal among the slots riding the same host
        // ("slot 2 of 4"), which no single role carries. This mirrors the model
        // the list already renders rather than adding an App member.
        Item {
            visible: false

            Repeater {
                id: slotIndexRepeater
                model: App.slotModel
                delegate: Item {
                    required property string slotId
                    required property string boundConnectionId
                }
            }
        }

        // LiveStat owns the rate formatter (nothing else in the app formats a
        // rate); the wire label is a plain string, so it borrows this instance.
        Kit.LiveStat { id: rateFormat; visible: false }
    }

    // ---- Row context menu ---------------------------------------------------
    Menu {
        id: rowMenu

        background: Rectangle {
            color: Theme.surface
            border.width: 1
            border.color: Theme.outline
            radius: Tokens.radiusButton
        }

        Repeater {
            model: page.rowMenuItems(page.currentBound, page.currentRemappable)
            delegate: MenuItem {
                id: menuEntry
                required property var modelData
                text: menuEntry.modelData.text

                contentItem: Text {
                    text: menuEntry.modelData.text
                    font.pixelSize: Tokens.textSummary
                    color: Theme.onSurface
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: menuEntry.highlighted ? Theme.primaryHover : "transparent"
                    radius: Tokens.radiusChip
                }
                onTriggered: page.runRowAction(menuEntry.modelData.key)
            }
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
            spacing: Tokens.s4

            Kit.LiveStat {
                text: qsTr("events/s %1   sends/s %2")
                          .arg(App.eventsPerSec).arg(App.sendsPerSec)
            }
            Item { Layout.fillWidth: true }
            Kit.LiveStat {
                text: qsTr("total %1").arg(App.totalSent)
            }
        }
    }

    // ---- Navigation ---------------------------------------------------------

    function setCurrentRow(slotId, slotName, bound, remappable) {
        page.currentSlotId = slotId;
        page.currentSlotName = slotName;
        page.currentBound = bound;
        page.currentRemappable = remappable;
    }

    function openBindingEditor(slotId) {
        if (!page.shellApi)
            return;
        page.shellApi.pushDetail(Qt.resolvedUrl("ConfigureBindingPage.qml"),
                                 qsTr("Configure binding"), { slotId: slotId });
    }

    function editCurrentRow() {
        if (page.currentBound && page.currentSlotId.length > 0)
            page.openBindingEditor(page.currentSlotId);
        else if (page.currentSlotId.length > 0 && page.shellApi)
            page.shellApi.openSetupWizard(page.currentSlotId);
    }

    function popupRowMenu() {
        if (page.currentSlotId.length === 0)
            return;
        rowMenu.popup();
    }

    // The row menu, built per row so an action that cannot apply is absent
    // rather than a dead entry.
    function rowMenuItems(bound, remappable) {
        var items = [];
        if (bound) {
            items.push({ key: "edit", text: qsTr("Configure binding…") });
            items.push({ key: "unbind", text: qsTr("Unbind") });
        } else {
            items.push({ key: "bind", text: qsTr("Bind…") });
        }
        if (remappable)
            items.push({ key: "controls", text: qsTr("Configure controls…") });
        items.push({ key: "deadzones", text: qsTr("Dead zones…") });
        return items;
    }

    function runRowAction(key) {
        if (page.currentSlotId.length === 0 || !page.shellApi)
            return;
        if (key === "edit") {
            page.openBindingEditor(page.currentSlotId);
        } else if (key === "bind") {
            page.shellApi.openSetupWizard(page.currentSlotId);
        } else if (key === "unbind") {
            App.unbindSlot(page.currentSlotId);
            // Unbind is confirm-free; the toast is the receipt.
            if (page.shellApi.toast)
                page.shellApi.toast(qsTr("Binding removed"), "success");
        } else if (key === "controls") {
            page.shellApi.pushDetail(Qt.resolvedUrl("ControlsRemapPage.qml"),
                                     qsTr("Configure controls"),
                                     { slotId: page.currentSlotId,
                                       slotName: page.currentSlotName });
        } else if (key === "deadzones") {
            page.shellApi.pushDetail(Qt.resolvedUrl("DeadzoneSettingsPage.qml"),
                                     qsTr("Dead zones & motion"), {});
        }
    }

    // ---- Helpers (presentation only; wording owned by this page) ------------

    // %n so each language picks its own plural form — Bosnian needs three, and an
    // explicit singular/plural pair can only ever express two. English is a
    // catalogue like any other (translations/dish_en.ts), not the raw fallback.
    function controllersStreamingText(n) {
        return qsTr("%n controllers streaming", "", n);
    }
    function satellitesOnlineText(n) {
        return qsTr("%n satellites online", "", n);
    }

    // On a Bluetooth pad a status-4 ("wired") reading is the HOST battery
    // substitute and would contradict the wireless link — drop it rather than
    // show it.
    function padSubText(row) {
        if (row.registering)
            return qsTr("Registering controller…");
        var parts = [];
        parts.push(row.bluetooth ? qsTr("Bluetooth") : qsTr("USB"));
        if (row.batteryKnown && !(row.bluetooth && row.batteryStatus === 4))
            parts.push(page.batteryText(row.batteryLevel, row.batteryStatus));
        if (!row.bound)
            parts.push(qsTr("Unbound"));
        return parts.join(" · ");
    }

    // The satellite node's sub-line: the address, then the slot this binding
    // actually occupies. The ordinal is never asserted before the bind lands.
    function satSubText(row, epoch) {
        if (!row.bound)
            return row.satIp;
        var ordinal = page.slotOrdinal(row.slotId, row.boundConnectionId, epoch);
        if (ordinal <= 0)
            return row.satIp;
        return row.satIp + " · " + qsTr("slot %1 of %2")
                                       .arg(ordinal).arg(App.hostSlotCapacity());
    }

    // Position of `slotId` among the slots riding `connectionId`, 1-based.
    // `epoch` is read by the caller's binding so the join re-runs when the slot
    // list moves (the index mirror carries no NOTIFY of its own).
    function slotOrdinal(slotId, connectionId, epoch) {
        if (connectionId.length === 0)
            return 0;
        var ordinal = 0;
        for (var i = 0; i < slotIndexRepeater.count; ++i) {
            var entry = slotIndexRepeater.itemAt(i);
            if (!entry || entry.boundConnectionId !== connectionId)
                continue;
            ++ordinal;
            if (entry.slotId === slotId)
                return ordinal;
        }
        return 0;
    }

    // Battery wording (2=charging, 3=full, 4=wired; batteryKnown
    // gates the unknown 255 out).
    function batteryText(level, status) {
        if (status === 4) { return qsTr("Battery wired"); }
        if (status === 2) { return qsTr("Battery %1% ↑").arg(level); }
        if (status === 3) { return qsTr("Battery full"); }
        return qsTr("Battery %1%").arg(level);
    }

    // The wire's mono label: "1000 Hz · ~3.4 ms" on a live link (whichever
    // halves are measured), "idle" on a dead one. `~` marks a derived figure —
    // always on latency, on Hz only when the rate is not continuously measured.
    function wireLabel(row) {
        if (!row.live)
            return qsTr("idle");
        var parts = [];
        if (row.gamepadHzShown) {
            var rate = rateFormat.rateText(row.gamepadHz, row.gamepadHzLive);
            if (rate.length > 0)
                parts.push(rate);
        }
        if (row.showLatency)
            parts.push(row.satLatencyText);
        return parts.length > 0 ? parts.join(" · ") : qsTr("live");
    }

    // ---- The binding strip --------------------------------------------------
    // Anything that cannot carry is still NAMED, with the layer that stops it.
    // `epoch` enlists the state graph so the invokable reads re-evaluate.
    function stripChips(row, epoch) {
        var chips = [];
        var direct = row.pathPhase === "direct";
        var dz = page.deadzoneRowFor(row.slotId);
        var motionOn = dz ? dz.forwardMotion : true;

        if (row.emulateName.length > 0) {
            chips.push({ text: qsTr("as %1").arg(row.emulateName),
                         tone: Kit.CapabilityChip.Present, reason: "" });
        }
        chips.push(direct
                   ? { text: qsTr("direct · raw HID"),
                       tone: Kit.CapabilityChip.Present, reason: "" }
                   : { text: qsTr("standard"), tone: Kit.CapabilityChip.Neutral,
                       reason: qsTr("Standard mode can’t carry it — switch to Direct.") });

        if (!row.hasMotion) {
            chips.push({ text: qsTr("no gyro · pad"), tone: Kit.CapabilityChip.Absent,
                         reason: qsTr("No gyro on this controller.") });
        } else if (!direct) {
            chips.push({ text: qsTr("no gyro · link"), tone: Kit.CapabilityChip.Absent,
                         reason: qsTr("Standard mode can’t carry it — switch to Direct.") });
        } else if (!motionOn) {
            chips.push({ text: qsTr("gyro off"), tone: Kit.CapabilityChip.Neutral,
                         reason: qsTr("Motion forwarding is off for this device.") });
        } else {
            chips.push({ text: qsTr("gyro on"), tone: Kit.CapabilityChip.Present, reason: "" });
        }

        if (!row.hasTouchpad) {
            chips.push({ text: qsTr("no touchpad · pad"), tone: Kit.CapabilityChip.Absent,
                         reason: qsTr("No touchpad on this controller.") });
        } else if (!direct) {
            chips.push({ text: qsTr("no touchpad · link"), tone: Kit.CapabilityChip.Absent,
                         reason: qsTr("Standard mode can’t carry it — switch to Direct.") });
        } else {
            var mode = App.touchpadModeFor(row.boundConnectionId);
            chips.push(mode === "off"
                       ? { text: qsTr("touchpad off"), tone: Kit.CapabilityChip.Neutral,
                           reason: qsTr("Touchpad routing is off for this binding.") }
                       : { text: mode === "mouse" ? qsTr("touchpad → mouse")
                                                  : qsTr("touchpad → pad"),
                           tone: Kit.CapabilityChip.Present, reason: "" });
        }

        chips.push(App.rumbleEnabledFor(row.slotId)
                   ? { text: qsTr("rumble on"), tone: Kit.CapabilityChip.Present, reason: "" }
                   : { text: qsTr("rumble off"), tone: Kit.CapabilityChip.Neutral,
                       reason: qsTr("Rumble is off for this binding.") });

        if (!row.hasLightbar) {
            chips.push({ text: qsTr("no lightbar · pad"), tone: Kit.CapabilityChip.Absent,
                         reason: qsTr("No lightbar on this controller.") });
        } else if (!App.lightbarFollowGame) {
            chips.push({ text: qsTr("lightbar off"), tone: Kit.CapabilityChip.Neutral,
                         reason: qsTr("Lightbar following is off in Settings.") });
        } else {
            chips.push({ text: qsTr("lightbar on"), tone: Kit.CapabilityChip.Present, reason: "" });
        }

        if (dz) {
            chips.push({ text: qsTr("dead zones stick %1% · trigger %2%")
                                   .arg(page.percentOf(dz.stickFlat, page.stickRange))
                                   .arg(page.percentOf(dz.triggerFlat, page.triggerRange)),
                         tone: Kit.CapabilityChip.Neutral, reason: "" });
        }
        return chips;
    }

    // The dead-zone row for a slot. The slot id IS the SDL bridge device id, so
    // the two lists join directly; a USB-direct synthetic pad has no row.
    function deadzoneRowFor(slotId) {
        for (var i = 0; i < page.deadzoneRows.length; ++i) {
            if (page.deadzoneRows[i].id === slotId)
                return page.deadzoneRows[i];
        }
        return null;
    }

    // Raw axis units presented as a share of full travel (the same mapping the
    // Dead zones page's sliders use).
    function percentOf(raw, range) {
        return Math.round(raw / range * 100);
    }

    // Localized chip text/tone for a link-state token — the same ladders the
    // Connections page renders from the identical tokens.
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
