// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wizard page 1 — Input. Which pad, and how Dish should read it. WAITING (empty
// slotModel) and the picker are one page, not two: waiting is a full step with
// real advice. Nothing here writes — the path is a DRAFT value the last step
// applies, so setSlotPath is never called from this file.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit

ColumnLayout {
    id: page

    property BindingDraft draft

    // Home ▸ Bind… pre-answers this page. Held as an id rather than written into
    // the draft by the caller, because only the row itself knows the pad's name,
    // transport and claimability.
    property string seedSlotId: ""

    // Republished so the banner and the review card read the adopted pad without
    // re-walking the model.
    signal padAdopted(var info)

    // ── The wizard's step contract ──────────────────────────────────────────
    readonly property bool waiting: App.slotModel.count === 0
    readonly property bool canAdvance: !page.waiting && page.draft.hasInput
    readonly property string primaryLabel: qsTr("Continue ›")
    readonly property string hint: page.waiting ? qsTr("Nothing is sent until the last step.") : ""

    function primaryActivated() {
        return true;
    }

    // ── The adopted pad's own facts ─────────────────────────────────────────
    property bool padBluetooth: false
    property bool padClaimable: false
    property bool padVerified: false

    readonly property bool pathIsAQuestion: page.padClaimable && !page.padBluetooth

    spacing: Tokens.s6

    // Kit.LiveStat owns the one rate formatter in the app; this hidden instance
    // is borrowed for it (an invisible child is skipped by the layout).
    Kit.LiveStat {
        id: rateFormat
        visible: false
    }

    function padSubtitle(bluetooth, claimable, hz, hzLive) {
        const parts = [bluetooth ? qsTr("Bluetooth") : qsTr("USB")];
        if (!bluetooth && claimable)
            parts.push(page.draft.desiredPath === "direct" ? qsTr("Direct") : qsTr("Standard"));
        const rate = rateFormat.rateText(hz, hzLive);
        if (rate.length > 0)
            parts.push(rate);
        return parts.join(" · ");
    }

    // The path is seeded from THAT PAD's own current setting, never from the
    // previously adopted one: a slot already running Direct stays Direct, and
    // downgrading it silently made Review disagree with the Home row it edits.
    // Bluetooth cannot carry Direct at all, so `usable` still has the last word.
    function adopt(slotId, name, bluetooth, claimable, currentPath, hz, hzLive,
                   motion, touchpad, lightbar) {
        const usable = claimable && !bluetooth;
        page.padBluetooth = bluetooth;
        page.padClaimable = usable;
        page.padVerified = App.isVerifiedModel(slotId);
        page.draft.chooseInput(slotId, name,
                               usable && currentPath === "direct" ? "direct" : "standard",
                               usable);
        // Motion is a persisted per-pad preference (Dead zones writes it too);
        // seeding from the default would silently re-enable a gyro turned off.
        page.draft.setMotion(App.motionEnabledFor(slotId));
        // Same rule for the audio pair: a re-run of the wizard over a pad whose
        // mic the user enabled must not silently reset it — and the other way
        // round, which for a microphone is the reset that matters.
        page.draft.setMic(App.micEnabledFor(slotId));
        page.draft.setSpeaker(App.speakerEnabledFor(slotId));
        page.padAdopted({
            "name": name,
            "transport": bluetooth ? qsTr("Bluetooth") : qsTr("USB"),
            "bluetooth": bluetooth,
            "claimable": usable,
            "verified": page.padVerified,
            "hz": hz,
            "hzLive": hzLive,
            "hasMotion": motion,
            "hasTouchpad": touchpad,
            "hasLightbar": lightbar
        });
    }

    // ── Head ────────────────────────────────────────────────────────────────
    Label {
        text: page.waiting ? qsTr("Plug your controller in") : qsTr("Which controller?")
        color: Theme.onSurface
        font.pixelSize: Tokens.textStatus
        font.bold: true
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        text: page.waiting
            ? qsTr("Into any USB port, or turn on a pad already paired to this machine. The system sees it, and it appears here — there is no list to add it to.")
            : qsTr("Each is drawn as the node it will become on Home, with what it can carry beside it.")
        color: Theme.muted
        font.pixelSize: Tokens.textSummary
        lineHeight: 1.5
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // ── WAITING ─────────────────────────────────────────────────────────────
    Kit.DishProgressBar {
        visible: page.waiting
        indeterminate: true
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2
    }

    Kit.Callout {
        visible: page.waiting
        tone: Kit.Callout.Info
        text: qsTr("A wired pad is the safest first run: it can use the Direct path, and it can’t run out of battery mid-setup.")
        Layout.fillWidth: true
    }

    // ── DETECTED — the pad picker ───────────────────────────────────────────
    // A model-backed list, not a Repeater over a snapshot: the slot model emits
    // scoped dataChanged, so a 1 Hz telemetry tick does not reset the picker.
    ListView {
        id: padList
        visible: !page.waiting
        interactive: false
        spacing: Tokens.s3
        model: App.slotModel
        Layout.fillWidth: true
        // The layout owns the height: a content-sized, non-interactive list so
        // the page's own scroller is the only scroll region.
        Layout.preferredHeight: contentHeight

        delegate: Kit.SelectRow {
            id: padRow

            required property int index
            required property string slotId
            required property string name
            required property bool bluetooth
            required property bool pathSupported
            required property string desiredPath
            required property bool hasMotion
            required property bool hasTouchpad
            required property bool hasLightbar
            required property bool hasRumble
            required property int gamepadHz
            required property bool gamepadHzLive

            readonly property bool seedMatch: page.seedSlotId.length > 0
                                              && page.seedSlotId === padRow.slotId

            width: padList.width
            selected: page.draft.slotId === padRow.slotId
            title: padRow.name
            subtitle: page.padSubtitle(padRow.bluetooth, padRow.pathSupported,
                                       padRow.gamepadHz, padRow.gamepadHzLive)

            function adoptThis() {
                page.adopt(padRow.slotId, padRow.name, padRow.bluetooth, padRow.pathSupported,
                           padRow.desiredPath, padRow.gamepadHz, padRow.gamepadHzLive,
                           padRow.hasMotion, padRow.hasTouchpad, padRow.hasLightbar);
            }

            onPicked: padRow.adoptThis()

            // The seed can land before or after this delegate exists (initial
            // properties versus delegate creation), so both orders adopt.
            onSeedMatchChanged: if (padRow.seedMatch) padRow.adoptThis()

            // The first row holds the draft until the seeded row claims it, and
            // keeps it when the seeded pad is no longer plugged in.
            Component.onCompleted: {
                if (padRow.seedMatch
                        || (padRow.index === 0 && page.draft.slotId.length === 0)) {
                    padRow.adoptThis();
                }
            }

            // What this pad can carry, always drawn — an absent capability is
            // an outlined chip with the negated label, never a missing one.
            Flow {
                spacing: Tokens.s2

                Kit.CapabilityChip {
                    text: padRow.hasRumble ? qsTr("Rumble") : qsTr("No rumble")
                    tone: padRow.hasRumble ? Kit.CapabilityChip.Present : Kit.CapabilityChip.Absent
                }
                Kit.CapabilityChip {
                    text: padRow.hasMotion ? qsTr("Gyro") : qsTr("No gyro")
                    tone: padRow.hasMotion ? Kit.CapabilityChip.Present : Kit.CapabilityChip.Absent
                }
                Kit.CapabilityChip {
                    text: padRow.hasTouchpad ? qsTr("Touchpad") : qsTr("No touchpad")
                    tone: padRow.hasTouchpad ? Kit.CapabilityChip.Present
                                             : Kit.CapabilityChip.Absent
                }
                Kit.CapabilityChip {
                    visible: padRow.hasLightbar
                    text: qsTr("Lightbar")
                    tone: Kit.CapabilityChip.Present
                }
            }
        }
    }

    // ── DETECTED — how Dish should read it ──────────────────────────────────
    // One-option questions are not questions: a Bluetooth pad or a pad the system
    // will not release states the fact instead of asking about it.
    Kit.Eyebrow {
        visible: !page.waiting && page.pathIsAQuestion
        mutedTone: true
        text: qsTr("How should Dish read it?")
        Layout.topMargin: Tokens.s2
    }

    RowLayout {
        visible: !page.waiting && page.pathIsAQuestion
        spacing: Tokens.s5
        Layout.fillWidth: true

        Kit.OptionCard {
            selected: page.draft.desiredPath === "standard"
            title: qsTr("Standard")
            body: qsTr("The system gamepad layer. No prompt.")
            badgeText: qsTr("Recommended")
            badgeTone: Kit.CapabilityChip.Ok
            Layout.fillWidth: true
            onClicked: page.draft.choosePath("standard")
        }
        Kit.OptionCard {
            selected: page.draft.desiredPath === "direct"
            title: qsTr("Direct")
            body: qsTr("Raw HID claim. Lowest latency.")
            // The trust judgement belongs to the path that carries the risk.
            badgeText: page.padVerified ? "" : qsTr("Layout guessed")
            badgeTone: Kit.CapabilityChip.Warn
            Layout.fillWidth: true
            onClicked: page.draft.choosePath("direct")
        }
    }

    Kit.Callout {
        visible: !page.waiting && page.pathIsAQuestion && !page.padVerified
                 && page.draft.desiredPath === "direct"
        tone: Kit.Callout.Warning
        text: qsTr("Not recognised — Direct would guess this pad’s layout. Try it, then switch if it feels off.")
        Layout.fillWidth: true
    }

    Label {
        visible: !page.waiting && !page.pathIsAQuestion
        text: page.padBluetooth ? qsTr("Read over Bluetooth — the system gamepad layer.")
                                : qsTr("Read through the system gamepad layer.")
        color: Theme.mutedStrong
        font.pixelSize: Tokens.textSummary
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2
    }

    Item {
        Layout.fillHeight: true
        Layout.minimumHeight: Tokens.s5
    }

    // ── The two reasons a first run sees nothing ────────────────────────────
    // Bluetooth off or absent is a real state, and it never blocks Continue:
    // USB still works.
    Kit.Callout {
        visible: page.waiting && !App.bluetoothPresent
        tone: Kit.Callout.Warning
        glyph: "bluetooth-disabled"
        text: qsTr("This machine has no Bluetooth adapter — plug a pad in over USB.")
        Layout.fillWidth: true
    }
    Kit.Callout {
        visible: page.waiting && App.bluetoothPresent && !App.bluetoothEnabled
        tone: Kit.Callout.Warning
        glyph: "bluetooth-off"
        text: qsTr("Bluetooth is off on this machine. Turn it on in your system settings to use a wireless pad, or plug one in over USB.")
        Layout.fillWidth: true

        Kit.DishButton {
            text: qsTr("Open Bluetooth settings ↗")
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            onClicked: App.openBluetoothSettings()
        }
    }
    Kit.Callout {
        visible: page.waiting && App.bluetoothPresent && App.bluetoothEnabled
        tone: Kit.Callout.Warning
        text: qsTr("Nothing appearing? A pad the system itself doesn’t list won’t reach Dish either — check your desktop’s own device settings first.")
        Layout.fillWidth: true
    }
}
