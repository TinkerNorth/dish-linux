// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wizard page 3 — Binding · type. What the PC should see. Every type card shares
// one capability table so the types are actually comparable. A row reads Pending
// whenever the host or its catalog is unresolved — a cross is never drawn from a
// catalog we could not read, and a type is never guessed.

// Bound: the card delegate reads the outer `page` id alongside its modelData.
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
    readonly property bool canAdvance: page.draft.hasType && page.types.length > 0
    readonly property string primaryLabel: qsTr("Continue ›")
    readonly property string hint: page.draft.hostName.length > 0
                                   ? qsTr("Types offered by %1’s catalog.").arg(page.draft.hostName)
                                   : ""

    function primaryActivated() {
        return true;
    }

    function activated() {
        // Keyed on the DESTINATION, never on the pad: the pad has no binding
        // yet, and the slot-keyed read resolves through hub_->bindings().
        if (page.draft.hasDestination)
            App.refreshEmulateForHost(page.draft.hostId);
        page.reload();
    }

    // ── Catalog state ───────────────────────────────────────────────────────
    property var types: []
    // The host's own pick for this pad — the pre-selection and the Best fit badge.
    property int bestFitType: -1

    readonly property bool loadingOnly: App.emulateLoading && page.types.length === 0
    // A failure with a cache behind it is silent: the cached types resolve the
    // draft and the user has nothing to act on.
    readonly property bool failedOnly: !App.emulateLoading && App.emulateError.length > 0
                                       && page.types.length === 0

    function reload() {
        if (!page.draft.hasDestination) {
            page.types = [];
            return;
        }
        page.types = App.emulateTypesForHost(page.draft.hostId);
        page.bestFitType = App.emulateCurrentTypeForHost(page.draft.hostId, page.draft.slotId);
        if (page.draft.type >= 0 || page.types.length === 0)
            return;
        // Pre-select the host's own answer. This is not a guess: an unresolved
        // catalog vends no types at all.
        for (let i = 0; i < page.types.length; ++i) {
            if (page.types[i].type === page.bestFitType) {
                page.draft.chooseType(page.types[i].type, page.types[i].name);
                return;
            }
        }
    }

    // `revision` is named at the call site so the draft is a binding dependency
    // — a plain function call is not one.
    function typeRows(candidateType, revision) {
        const all = page.draft.rowsFor(candidateType);
        const wanted = [
            { "feature": "rumble", "name": qsTr("Rumble") },
            { "feature": "motion", "name": qsTr("Motion / gyro") },
            { "feature": "touchpad", "name": qsTr("Touchpad") }
        ];
        const out = [];
        for (let i = 0; i < wanted.length; ++i) {
            for (let j = 0; j < all.length; ++j) {
                if (all[j].feature !== wanted[i].feature)
                    continue;
                const row = all[j];
                row.name = wanted[i].name;
                // The compact table has no room for a reason line; the Feel
                // step and Configure binding carry the sentences.
                row.why = "";
                out.push(row);
                break;
            }
        }
        return out;
    }

    // Up/Down move AND select: a radio group has one value, so focus and
    // selection are the same thing.
    function stepSelection(delta) {
        if (page.types.length === 0)
            return;
        let index = -1;
        for (let i = 0; i < page.types.length; ++i) {
            if (page.types[i].type === page.draft.type) {
                index = i;
                break;
            }
        }
        const next = index < 0 ? 0 : index + delta;
        if (next >= 0 && next < page.types.length)
            page.draft.chooseType(page.types[next].type, page.types[next].name);
    }

    spacing: Tokens.s6

    Connections {
        target: App
        function onEmulateStateChanged() { page.reload(); }
    }

    // ── Head ────────────────────────────────────────────────────────────────
    Label {
        text: qsTr("How should the PC see it?")
        color: Theme.onSurface
        font.pixelSize: Tokens.textStatus
        font.bold: true
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        text: qsTr("Pick the controller the PC should report. Each unlocks different extras — this pad limits all three the same way.")
        color: Theme.muted
        font.pixelSize: Tokens.textSummary
        lineHeight: 1.5
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // ── Catalog loading — never a guessed default ───────────────────────────
    Kit.LoadingSpinner {
        visible: page.loadingOnly
        running: page.loadingOnly
        text: page.draft.hostName.length > 0
              ? qsTr("Reading %1’s controller catalog…").arg(page.draft.hostName)
              : qsTr("Reading the controller catalog…")
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s8
    }

    // ── Catalog failed with nothing cached — tap to retry ───────────────────
    Kit.ErrorBanner {
        visible: page.failedOnly
        text: App.emulateError
        detail: qsTr("Dish cannot offer a type it has not read. Retry once the host is reachable.")
        showRetry: true
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s2

        onRetryRequested: page.activated()
    }

    // ── The shared header, inset to match the cards' own padding ────────────
    Kit.CapabilityTable {
        visible: page.types.length > 0
        rows: []
        showHeader: true
        compact: true
        Layout.fillWidth: true
        Layout.leftMargin: Tokens.s5
        Layout.rightMargin: Tokens.s5
        Layout.topMargin: Tokens.s2
    }

    // ── One card per catalog type ───────────────────────────────────────────
    Repeater {
        model: page.types

        delegate: AbstractButton {
            id: typeCard

            required property var modelData

            readonly property bool selected: page.draft.type === typeCard.modelData.type
            readonly property bool bestFit: typeCard.modelData.type === page.bestFitType

            Layout.fillWidth: true
            padding: Tokens.s5
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true

            Accessible.role: Accessible.RadioButton
            Accessible.name: typeCard.modelData.name
            Accessible.checked: typeCard.selected

            onClicked: page.draft.chooseType(typeCard.modelData.type, typeCard.modelData.name)
            Keys.onUpPressed: page.stepSelection(-1)
            Keys.onDownPressed: page.stepSelection(1)

            background: Item {
                // Selection is 1px accent over the accent fill. Never 2px: the
                // only 2px border in the app is a capture-armed control.
                Rectangle {
                    anchors.fill: parent
                    radius: Tokens.radiusCard
                    color: typeCard.selected ? Theme.primaryFill
                         : typeCard.hovered ? Theme.primaryHover : "transparent"
                    border.width: 1
                    border.color: typeCard.selected ? Theme.primary : Theme.outline

                    Behavior on color {
                        enabled: !Tokens.reducedMotion
                        ColorAnimation { duration: Tokens.durFast }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: Tokens.radiusCard + 2
                    visible: typeCard.visualFocus
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.focusRing
                }
            }

            contentItem: ColumnLayout {
                spacing: Tokens.s2

                RowLayout {
                    spacing: Tokens.s5
                    Layout.fillWidth: true

                    Kit.RadioMark { selected: typeCard.selected }

                    Label {
                        text: typeCard.modelData.name
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Kit.CapabilityChip {
                        visible: typeCard.bestFit
                        text: qsTr("Best fit")
                        tone: Kit.CapabilityChip.Ok
                    }
                }

                Rectangle {
                    implicitHeight: 1
                    color: Theme.outlineSubtle
                    Layout.fillWidth: true
                    Layout.topMargin: Tokens.s1
                }

                Kit.CapabilityTable {
                    rows: page.typeRows(typeCard.modelData.type, page.draft.revision)
                    showHeader: false
                    compact: true
                    Layout.fillWidth: true
                }
            }
        }
    }

    Item {
        Layout.fillHeight: true
        Layout.minimumHeight: Tokens.s5
    }
}
