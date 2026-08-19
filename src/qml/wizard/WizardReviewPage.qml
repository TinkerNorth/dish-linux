// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wizard page 5 — Review and bind. The banner IS the review; this card is only
// what the banner cannot draw — what the wire carries in each direction, absent
// capabilities still named. Bind is the first and only write in the flow: pages
// 1-4 call no setter, and the surface stays open on failure, draft intact.

// Bound: the chip delegates read the outer `page` id alongside their modelData.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit

ColumnLayout {
    id: page

    property BindingDraft draft

    // ── The wizard's step contract ──────────────────────────────────────────
    readonly property bool canAdvance: page.draft.hasInput && page.draft.hasDestination
                                       && page.draft.hasType
    readonly property string primaryLabel: qsTr("Bind")
    readonly property string hint: qsTr("This is the first thing that is sent.")

    // Returns false so the wizard does not step: the apply overlay owns the
    // screen from here, and the outcome decides what happens.
    function primaryActivated() {
        App.applyBinding(page.draft.slotId, page.draft.hostId, page.draft.type,
                         page.draft.desiredPath, page.draft.motionOn, page.draft.rumbleOn,
                         page.draft.touchpadMode);
        return false;
    }

    function activated() {
        page.accounting += 1;
    }

    // ── What the wire carries ───────────────────────────────────────────────
    // `revision` is read so the solver re-runs when the draft moves.
    readonly property var solved: page.draft.revision >= 0 ? page.draft.capabilityRows() : []

    // Bumped to re-run the host accounting below: an invokable call is not a
    // binding dependency.
    property int accounting: 0

    readonly property string displacedPad: page.accounting >= 0
                                         ? App.displacedSlotName(page.draft.hostId) : ""

    readonly property var sendChips: page.buildChips(["gamepad", "motion", "touchpad", "mouse"])
    readonly property var getChips: page.buildChips(["rumble", "lightbar"])

    // The banner's wire suffix, in the app's lowercase telemetry voice
    // ("as Xbox 360 · rumble").
    readonly property string extrasSummary: {
        const parts = [];
        if (page.verdictOf("rumble") === "available")
            parts.push(qsTr("rumble"));
        if (page.verdictOf("motion") === "available")
            parts.push(qsTr("gyro"));
        if (page.draft.touchpadMode === 1 && page.verdictOf("touchpad") === "available")
            parts.push(qsTr("touchpad"));
        else if (page.draft.touchpadMode === 2 && page.verdictOf("mouse") === "available")
            parts.push(qsTr("mouse"));
        if (page.verdictOf("lightbar") === "available")
            parts.push(qsTr("lightbar"));
        return parts.join(" · ");
    }

    function rowFor(feature) {
        for (let i = 0; i < page.solved.length; ++i) {
            if (page.solved[i].feature === feature)
                return page.solved[i];
        }
        return null;
    }

    function verdictOf(feature) {
        const row = page.rowFor(feature);
        return row ? row.verdict : "pending";
    }

    function positiveName(feature) {
        switch (feature) {
        case "motion":
            return qsTr("Gyro");
        case "touchpad":
            return qsTr("Touchpad");
        case "mouse":
            return qsTr("Mouse");
        case "rumble":
            return qsTr("Rumble");
        case "lightbar":
            return qsTr("Lightbar");
        }
        return qsTr("Controller");
    }

    function negativeName(feature) {
        switch (feature) {
        case "motion":
            return qsTr("No gyro");
        case "touchpad":
            return qsTr("No touchpad");
        case "mouse":
            return qsTr("No mouse");
        case "rumble":
            return qsTr("No rumble");
        case "lightbar":
            return qsTr("No lightbar");
        }
        return qsTr("Controller");
    }

    function chipText(feature, verdict) {
        if (feature === "gamepad")
            return qsTr("Controller");
        if (verdict === "available")
            return page.positiveName(feature);
        // "Off" is the user's own answer and must never read as "unsupported".
        if (verdict === "off")
            return qsTr("%1 off").arg(page.positiveName(feature));
        if (verdict === "pending")
            return qsTr("%1 pending").arg(page.positiveName(feature));
        return page.negativeName(feature);
    }

    // An absent capability is still NAMED — a dimmed outlined pill, never a
    // missing one.
    function buildChips(features) {
        const out = [];
        for (let i = 0; i < features.length; ++i) {
            const feature = features[i];
            const verdict = page.verdictOf(feature);
            // The mouse is a routing of the touchpad: "No mouse" beside "No
            // touchpad" reports the same fact twice.
            if (feature === "mouse" && verdict !== "available")
                continue;
            out.push({
                "text": page.chipText(feature, verdict),
                "present": verdict === "available"
            });
        }
        return out;
    }

    spacing: Tokens.s6

    Connections {
        target: App
        function onStateChanged() { page.accounting += 1; }
    }

    // ── Head ────────────────────────────────────────────────────────────────
    Label {
        text: qsTr("Review and bind")
        color: Theme.onSurface
        font.pixelSize: Tokens.textStatus
        font.bold: true
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        text: qsTr("Everything above is decided. Bind draws the wire for real.")
        color: Theme.muted
        font.pixelSize: Tokens.textSummary
        lineHeight: 1.5
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // ── SENDS / GETS ────────────────────────────────────────────────────────
    Kit.Card {
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2

        contentItem: ColumnLayout {
            spacing: Tokens.s4

            RowLayout {
                spacing: Tokens.s6
                Layout.fillWidth: true

                Kit.Eyebrow {
                    mutedTone: true
                    text: qsTr("Sends")
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: Tokens.s1
                    Layout.minimumWidth: Tokens.s11
                }
                Flow {
                    spacing: Tokens.s2
                    Layout.fillWidth: true

                    Repeater {
                        model: page.sendChips

                        delegate: Kit.CapabilityChip {
                            id: sendChip
                            required property var modelData
                            text: sendChip.modelData.text
                            tone: sendChip.modelData.present ? Kit.CapabilityChip.Present
                                                             : Kit.CapabilityChip.Absent
                        }
                    }
                }
            }

            RowLayout {
                spacing: Tokens.s6
                Layout.fillWidth: true

                Kit.Eyebrow {
                    mutedTone: true
                    text: qsTr("Gets")
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: Tokens.s1
                    Layout.minimumWidth: Tokens.s11
                }
                Flow {
                    spacing: Tokens.s2
                    Layout.fillWidth: true

                    Repeater {
                        model: page.getChips

                        delegate: Kit.CapabilityChip {
                            id: getChip
                            required property var modelData
                            text: getChip.modelData.text
                            tone: getChip.modelData.present ? Kit.CapabilityChip.Present
                                                            : Kit.CapabilityChip.Absent
                        }
                    }
                }
            }
        }
    }

    // A full host is not a blocker, but it is not a surprise either: name the
    // pad that goes before the user presses Bind.
    Kit.Callout {
        visible: page.displacedPad.length > 0
        tone: Kit.Callout.Warning
        text: qsTr("%1 is full — %2 will be unbound when this binding lands.")
                .arg(page.draft.hostName).arg(page.displacedPad)
        Layout.fillWidth: true
    }

    Kit.Callout {
        tone: Kit.Callout.Info
        text: qsTr("Nothing on %1 has changed yet. Bind is the first and only write.")
                .arg(page.draft.hostName)
        Layout.fillWidth: true
    }

    Item {
        Layout.fillHeight: true
        Layout.minimumHeight: Tokens.s5
    }
}
