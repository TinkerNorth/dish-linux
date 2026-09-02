// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wizard page 4 — Binding · feel. All six rows render always; an unavailable one
// names the layer that refused it, and only the CONTROL dims — the reason is the
// part the user came for. Reasons come from BindingDraft.whyFor(), which
// Configure binding reads too, so one pad never gets two explanations.

// Bound: the row delegate reads the outer `page` id alongside its modelData.
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
    // Always: every row may be dimmed and the pad is still bindable.
    readonly property bool canAdvance: true
    readonly property string primaryLabel: qsTr("Continue ›")
    readonly property string hint: qsTr("%1 extras · %2 available")
                                     .arg(page.extras.length).arg(page.availableCount)

    function primaryActivated() {
        return true;
    }

    function activated() {
        // Nothing to fetch: the solver reads what pages 1-3 already resolved.
    }

    // ── The six extras ──────────────────────────────────────────────────────
    // `gated` marks the ones this binding actually owns a switch for. The light
    // bar is forwarded per-app, not per-binding, so its control reports rather
    // than sets.
    readonly property var extras: [
        {
            "feature": "rumble",
            "label": qsTr("Rumble"),
            "body": qsTr("The pad’s motors buzz on feedback from the game."),
            "segmented": false,
            "gated": true
        },
        {
            "feature": "motion",
            "label": qsTr("Motion"),
            "body": qsTr("Forwards this pad’s gyro to the game as motion."),
            "segmented": false,
            "gated": true
        },
        {
            "feature": "touchpad",
            "label": qsTr("Touchpad"),
            "body": qsTr("Send the touchpad as a touchpad, or route it as a mouse."),
            "segmented": true,
            "gated": true
        },
        {
            "feature": "lightbar",
            "label": qsTr("Lightbar"),
            "body": qsTr("The game drives the pad’s light bar. Lightbar forwarding is a global setting."),
            "segmented": false,
            "gated": false
        },
        {
            "feature": "mic",
            "label": qsTr("Microphone"),
            "body": qsTr("The pad’s mic carries your voice to the host. Off sends nothing."),
            "segmented": false,
            "gated": true
        },
        {
            "feature": "speaker",
            "label": qsTr("Controller sound"),
            "body": qsTr("Audio from the host plays on the pad’s speaker or headset."),
            "segmented": false,
            "gated": true
        }
    ]

    readonly property var touchpadOptions: [qsTr("Off"), qsTr("Pad"), qsTr("Mouse")]

    // Re-solved whenever the draft moves; a function call is not a dependency,
    // so `revision` is read to make one.
    readonly property var solved: page.draft.revision >= 0 ? page.draft.capabilityRows() : []

    readonly property int availableCount: {
        let count = 0;
        for (let i = 0; i < page.extras.length; ++i) {
            if (page.carries(page.extras[i].feature))
                count += 1;
        }
        return count;
    }

    function carries(feature) {
        return page.draft.layersCarry(page.solved, feature);
    }

    function rowFor(feature) {
        for (let i = 0; i < page.solved.length; ++i) {
            if (page.solved[i].feature === feature)
                return page.solved[i];
        }
        return null;
    }

    function bodyFor(descriptor) {
        if (page.carries(descriptor.feature))
            return descriptor.body;
        const row = page.rowFor(descriptor.feature);
        const why = row ? page.draft.whyFor(row) : "";
        return why.length > 0 ? why : descriptor.body;
    }

    function checkedFor(feature) {
        if (feature === "rumble")
            return page.draft.rumbleOn;
        if (feature === "motion")
            return page.draft.motionOn;
        if (feature === "mic")
            return page.draft.micOn;
        if (feature === "speaker")
            return page.draft.speakerOn;
        return App.lightbarFollowGame;
    }

    function applyGate(feature, on) {
        if (feature === "rumble")
            page.draft.setRumble(on);
        else if (feature === "motion")
            page.draft.setMotion(on);
        else if (feature === "mic")
            page.draft.setMic(on);
        else if (feature === "speaker")
            page.draft.setSpeaker(on);
    }

    spacing: Tokens.s6

    // ── Head ────────────────────────────────────────────────────────────────
    Label {
        text: qsTr("How should it feel?")
        color: Theme.onSurface
        font.pixelSize: Tokens.textStatus
        font.bold: true
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        text: qsTr("Only the extras your input and destination both support are shown.")
        color: Theme.muted
        font.pixelSize: Tokens.textSummary
        lineHeight: 1.5
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // ── The rows ────────────────────────────────────────────────────────────
    Repeater {
        model: page.extras

        delegate: ColumnLayout {
            id: feelRow

            required property var modelData

            readonly property bool available: page.carries(feelRow.modelData.feature)
            readonly property bool live: feelRow.available && feelRow.modelData.gated

            spacing: Tokens.s1
            Layout.fillWidth: true

            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("%1 — %2").arg(feelRow.modelData.label)
                                            .arg(page.bodyFor(feelRow.modelData))

            RowLayout {
                spacing: Tokens.s6
                Layout.fillWidth: true
                Layout.topMargin: Tokens.s3
                Layout.bottomMargin: Tokens.s2

                ColumnLayout {
                    spacing: Tokens.s1
                    Layout.fillWidth: true

                    Label {
                        text: feelRow.modelData.label
                        // Full strength even when the control is dead: the
                        // label is not the disabled thing.
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                        Layout.fillWidth: true
                    }
                    Label {
                        text: page.bodyFor(feelRow.modelData)
                        color: Theme.mutedStrong
                        font.pixelSize: Tokens.textMeta
                        lineHeight: 1.3
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                Kit.LabeledSwitch {
                    id: feelSwitch
                    visible: !feelRow.modelData.segmented
                    checked: page.checkedFor(feelRow.modelData.feature)
                    enabled: feelRow.live
                    opacity: feelRow.live ? 1.0 : Tokens.disabledOpacity
                    Layout.alignment: Qt.AlignVCenter

                    onToggled: function (on) {
                        page.applyGate(feelRow.modelData.feature, on);
                        // Re-arm the binding the user's toggle replaced, so a
                        // sanitise that rejects the change is not masked by a
                        // stale control.
                        feelSwitch.checked = Qt.binding(function () {
                            return page.checkedFor(feelRow.modelData.feature);
                        });
                    }
                }

                Kit.SegmentedControl {
                    visible: feelRow.modelData.segmented
                    options: page.touchpadOptions
                    value: page.touchpadOptions[page.draft.touchpadMode]
                    small: true
                    enabled: feelRow.live
                    Layout.alignment: Qt.AlignVCenter

                    onPicked: function (option) {
                        page.draft.setTouchpad(page.touchpadOptions.indexOf(option));
                    }
                }
            }

            Rectangle {
                implicitHeight: 1
                color: Theme.outlineSubtle
                Layout.fillWidth: true
            }
        }
    }

    Item {
        Layout.fillHeight: true
        Layout.minimumHeight: Tokens.s5
    }

    Kit.Callout {
        tone: Kit.Callout.Info
        text: qsTr("Nothing here is saved yet. Everything travels with the bind on the next step.")
        Layout.fillWidth: true
    }
}
