// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Controllers destination — the inventory of pads: everything that belongs
// to the DEVICE, nothing that belongs to a binding. Binding writes all live on
// Home and in the setup wizard; the card's one binding-aware element is the
// `Bound · <host> ›` hand-off into the binding editor.

// Bound: the slot delegate references the outer `page` id and its `required`
// model props statically.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: page
    title: qsTr("Controllers")

    // Rendered by AppShell, not by this body; the page owns only the wording.
    readonly property string headerTitle: qsTr("Controllers")
    // Each half is its own %n message: plural forms do not survive a join.
    readonly property string headerSub: qsTr("%n connected", "", App.slotCount)
                                        + " · " + qsTr("%n bound", "", App.boundSlotCount)
    readonly property string headerDot: App.slotCount > 0 ? "success" : "muted"

    // var-typed so the shell's dynamic `shellApi` resolves without lint warnings.
    readonly property var shellStack: StackView.view
    readonly property var shellApi: shellStack ? shellStack.shellApi : null

    // LiveStat owns the one rate formatter in the app; the composed meta line
    // borrows this instance for it.
    Kit.LiveStat { id: rateFormat; visible: false }

    // Centred in the content viewport: the wrapper spans the ScrollView's
    // height, and with the list hidden it is the only laid-out child.
    Item {
        visible: App.slotCount === 0
        width: parent ? parent.width : implicitWidth
        height: page.contentItem.height

        Kit.EmptyState {
            anchors.centerIn: parent
            width: parent.width
            glyph: "dish-off"
            title: qsTr("No controllers connected")
            body: qsTr("Plug in an Xbox, PlayStation, or generic pad over USB or Bluetooth — the system detects it and Dish lists it here automatically.")
            actionText: qsTr("Open Connections")
            showAction: true
            // Destination 2 is Connections (AppShell rail order).
            onActionRequested: if (page.shellApi) page.shellApi.selectDestination(2)
        }
    }

    ColumnLayout {
        id: bodyColumn
        visible: App.slotCount > 0
        width: parent.width
        spacing: Tokens.s5

        RowLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.SectionHeader { label: qsTr("Connected") }
            Item { Layout.fillWidth: true }
            Kit.LiveStat { text: qsTr("%n connected", "", App.slotCount) }
        }

        // Content-sized and inert: the page's single scroller owns all overflow.
        ListView {
            id: slotList
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            interactive: false
            spacing: Tokens.s5
            model: App.slotModel

            delegate: Kit.Card {
                id: card
                width: ListView.view ? ListView.view.width : implicitWidth

                required property string slotId
                required property string name
                required property bool bound
                required property string boundLabel
                required property bool registering
                required property bool live
                required property string dotColor
                required property bool bluetooth
                required property bool remappable
                required property bool hasMotion
                required property bool hasTouchpad
                required property bool hasLightbar
                required property int batteryLevel
                required property int batteryStatus
                required property bool batteryKnown
                required property bool batteryLow
                required property int gamepadHz
                required property bool gamepadHzLive
                required property bool gamepadHzShown
                required property int motionHz
                required property bool motionHzShown
                required property int pollHz
                required property bool pollHzShown
                required property string pathPhase
                required property string desiredPath
                required property bool pathSupported
                required property bool claimInProgress
                required property string directFailure

                readonly property string pathNote:
                    pathSupported ? page.pathNoteText(pathPhase, directFailure) : ""

                Accessible.role: Accessible.ListItem
                Accessible.name: card.bound
                                 ? qsTr("%1, bound to %2").arg(card.name).arg(card.boundLabel)
                                 : qsTr("%1, not bound").arg(card.name)

                // A Qt layout skips hidden items AND their spacing, so the
                // registering variant really replaces the card rather than
                // leaving a gap where the other bands were.
                contentItem: ColumnLayout {
                    spacing: Tokens.s5

                    RowLayout {
                        visible: card.registering
                        Layout.fillWidth: true
                        spacing: Tokens.s6

                        Kit.BrandGlyph {
                            // The shipped `*-animated` SVGs carry no <animate>
                            // element, so the bar below is the transient.
                            glyph: "dish"
                            Layout.preferredWidth: Tokens.glyphLg
                            Layout.preferredHeight: Tokens.glyphLg
                            Layout.alignment: Qt.AlignVCenter
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s1

                            Label {
                                text: card.name
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textBase
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: qsTr("Registering controller…")
                                color: Theme.mutedStrong
                                font.pixelSize: Tokens.textMeta
                            }
                        }
                    }
                    Kit.DishProgressBar {
                        visible: card.registering
                        indeterminate: true
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        visible: !card.registering
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.StatusDot {
                            token: card.dotColor
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Label {
                            text: card.name
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Kit.LiveStat {
                            live: card.live
                            text: page.metaText(card)
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Kit.DishButton {
                            visible: card.bound
                            text: qsTr("Bound · %1 ›").arg(card.boundLabel)
                            variant: Kit.DishButton.Outline
                            size: Kit.DishButton.Small
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: page.openBindingEditor(card.slotId)
                        }
                        Kit.LiveStat {
                            visible: !card.bound
                            text: qsTr("not bound")
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    Flow {
                        visible: !card.registering
                        Layout.fillWidth: true
                        spacing: Tokens.s3

                        Kit.CapabilityChip {
                            text: card.hasMotion ? qsTr("Gyro") : qsTr("No gyro")
                            tone: card.hasMotion ? Kit.CapabilityChip.Present
                                                 : Kit.CapabilityChip.Absent
                        }
                        Kit.CapabilityChip {
                            text: card.hasTouchpad ? qsTr("Touchpad") : qsTr("No touchpad")
                            tone: card.hasTouchpad ? Kit.CapabilityChip.Present
                                                   : Kit.CapabilityChip.Absent
                        }
                        // Named only when present: "no lightbar" is the normal
                        // case, not a finding.
                        Kit.CapabilityChip {
                            visible: card.hasLightbar
                            text: qsTr("Lightbar")
                            tone: Kit.CapabilityChip.Present
                        }
                        // On a Bluetooth pad a status-4 ("wired") reading is the
                        // HOST battery substitute, and would contradict the
                        // wireless link — drop it rather than show it.
                        Kit.CapabilityChip {
                            visible: card.batteryKnown
                                     && !(card.bluetooth && card.batteryStatus === 4)
                            text: page.batteryLabel(card.batteryLevel, card.batteryStatus)
                            tone: card.batteryLow ? Kit.CapabilityChip.Low
                                                  : Kit.CapabilityChip.Neutral
                        }

                        Kit.LiveStat {
                            visible: card.motionHzShown
                            live: card.live
                            text: qsTr("Motion %1 Hz").arg(card.motionHz)
                        }
                        Kit.LiveStat {
                            visible: card.pollHzShown
                            live: card.live && card.pathPhase === "direct"
                            text: qsTr("Poll %1 Hz").arg(card.pollHz)
                        }
                    }

                    RowLayout {
                        visible: !card.registering
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.DishButton {
                            text: qsTr("Dead zones…")
                            variant: Kit.DishButton.Outline
                            onClicked: page.openDeadzones()
                        }
                        // Raw-DirectInput pads only: slotRemap() returns {} for
                        // anything SDL already maps.
                        Kit.DishButton {
                            visible: card.remappable
                            text: qsTr("Remap…")
                            variant: Kit.DishButton.Outline
                            onClicked: page.openRemap(card.slotId, card.name)
                        }

                        Item { Layout.fillWidth: true }

                        // Hidden entirely, never disabled, for a pad the raw-HID
                        // path cannot claim (a Bluetooth pad never can).
                        Kit.Eyebrow {
                            visible: card.pathSupported
                            mutedTone: true
                            text: qsTr("Input path")
                            Layout.alignment: Qt.AlignVCenter
                        }
                        // Two segments only: `auto` is a setSlotPath INPUT and
                        // never appears in desiredPath, so it is never offered.
                        Kit.SegmentedControl {
                            visible: card.pathSupported
                            small: true
                            busy: card.claimInProgress
                            options: [qsTr("Standard"), qsTr("Direct")]
                            value: card.desiredPath === "direct" ? qsTr("Direct")
                                                                 : qsTr("Standard")
                            Layout.alignment: Qt.AlignVCenter
                            onPicked: function(option) {
                                App.setSlotPath(card.slotId,
                                                option === qsTr("Direct") ? "direct"
                                                                          : "standard");
                            }
                        }
                    }

                    // Every path failure is an inline reason. Never a toast.
                    RowLayout {
                        visible: !card.registering && card.pathSupported
                                 && (card.claimInProgress || card.pathNote.length > 0)
                        Layout.fillWidth: true
                        spacing: Tokens.s4

                        Kit.DishProgressBar {
                            visible: card.claimInProgress
                            indeterminate: true
                            Layout.preferredWidth: Tokens.s11 * 2
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Label {
                            visible: card.claimInProgress
                            text: qsTr("Claiming controller…")
                            color: Theme.mutedStrong
                            font.pixelSize: Tokens.textMeta
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Label {
                            visible: !card.claimInProgress && card.pathNote.length > 0
                            text: card.pathNote
                            color: page.pathNoteIsError(card.pathPhase, card.directFailure)
                                   ? Theme.error : Theme.warning
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }
                }
            }
        }
    }

    // Pinned outside the scroller so it holds while the slot list moves.
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
                text: qsTr("%n connected", "", App.slotCount)
                      + " · " + qsTr("%n bound", "", App.boundSlotCount)
            }
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

    function openBindingEditor(slotId) {
        if (!page.shellApi)
            return;
        page.shellApi.pushDetail(Qt.resolvedUrl("ConfigureBindingPage.qml"),
                                 qsTr("Configure binding"), { slotId: slotId });
    }

    // Needs slotId / slotName BEFORE load — pushDetail's initial-property map.
    function openRemap(slotId, slotName) {
        if (!page.shellApi)
            return;
        page.shellApi.pushDetail(Qt.resolvedUrl("ControlsRemapPage.qml"),
                                 qsTr("Configure controls"),
                                 { slotId: slotId, slotName: slotName });
    }

    function openDeadzones() {
        if (!page.shellApi)
            return;
        page.shellApi.pushDetail(Qt.resolvedUrl("DeadzoneSettingsPage.qml"),
                                 qsTr("Dead zones & motion"), {});
    }

    function metaText(row) {
        var parts = [];
        parts.push(row.bluetooth ? qsTr("Bluetooth") : qsTr("USB"));
        if (row.gamepadHzShown) {
            var rate = rateFormat.rateText(row.gamepadHz, row.gamepadHzLive);
            if (rate.length > 0)
                parts.push(rate);
        }
        return parts.join(" · ");
    }

    // Wire status codes: 2=charging, 3=full, 4=wired.
    function batteryLabel(level, status) {
        if (status === 4) { return qsTr("Battery wired"); }
        if (status === 2) { return qsTr("Battery %1% ↑").arg(level); }
        if (status === 3) { return qsTr("Battery full"); }
        return qsTr("Battery %1%").arg(level);
    }
    // The inline note for the non-happy path states; empty for the steady
    // routed / direct / claiming ones.
    function pathNoteText(phase, failure) {
        if (phase === "needsReplug") {
            return qsTr("Unplug and replug the controller to finish switching.");
        }
        if (phase === "restoreStuck") {
            return qsTr("Standard mode isn’t responding — switch to Direct, retry, or replug.");
        }
        if (failure === "permissionDenied") {
            return qsTr("Direct access denied — another app owns this device.");
        }
        if (failure === "busy") {
            return qsTr("Direct claim is busy — another app or driver holds the device.");
        }
        if (failure === "initFailed") {
            return qsTr("Direct claim couldn’t start the controller’s report stream.");
        }
        if (failure === "dropped") {
            return qsTr("The device dropped during the claim — a replug is needed.");
        }
        return "";
    }
    // Phase notes are warning-toned; only a claim failure reads as an error.
    function pathNoteIsError(phase, failure) {
        return phase !== "needsReplug" && phase !== "restoreStuck"
               && failure.length > 0;
    }
}
