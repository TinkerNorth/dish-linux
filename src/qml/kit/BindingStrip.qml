// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The binding, printed underneath. It never collapses to a bare count: reading
// the binding without pushing a page is the strip's entire job.
//
// The fit is measured from the laid-out chips' implicit widths and assigned
// imperatively. A binding would feed the measurement back into the layout it is
// measuring; `shownCount` is therefore a plain property, recomputed through
// Qt.callLater so the chips have settled before they are counted.

// Bound: the chip and popup delegates read the outer `strip` id alongside their
// required modelData.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

Item {
    id: strip

    // [{ text: string, tone: int /* CapabilityChip.Tone */, reason: string }]
    property var chips: []
    property bool showEdit: true
    // The parent's content width: what the overflow rule is written against.
    property int availableWidth: 0

    signal editRequested()

    readonly property int effectiveWidth: strip.availableWidth > 0 ? strip.availableWidth
                                                                   : strip.width
    // The chips take their own line rather than lose a single labelled fact.
    readonly property bool narrow: strip.effectiveWidth > 0
                                   && strip.effectiveWidth < Tokens.stackBreakpoint - 160

    readonly property int chipCount: strip.chips ? strip.chips.length : 0
    // Never bound — see the header.
    property int shownCount: 0
    readonly property int hiddenCount: Math.max(0, strip.chipCount - strip.shownCount)
    readonly property var hiddenChips: strip.chips ? strip.chips.slice(strip.shownCount) : []

    implicitHeight: layout.implicitHeight

    onChipsChanged: {
        // Fail open: show everything, then trim on the next tick. An unmeasured
        // strip that overflows for one frame beats one that renders nothing.
        strip.shownCount = strip.chipCount;
        Qt.callLater(strip.relayoutChips);
    }
    onWidthChanged: Qt.callLater(strip.relayoutChips)
    onAvailableWidthChanged: Qt.callLater(strip.relayoutChips)
    onNarrowChanged: Qt.callLater(strip.relayoutChips)
    Component.onCompleted: strip.relayoutChips()

    function chipBudget() {
        if (strip.narrow)
            return layout.width > 0 ? layout.width : strip.effectiveWidth;
        return inlineSlot.width;
    }

    function relayoutChips() {
        const total = strip.chipCount;
        if (total === 0) {
            strip.shownCount = 0;
            return;
        }
        const budget = strip.chipBudget();
        if (budget <= 0) {
            strip.shownCount = total;
            return;
        }
        let used = 0;
        let fit = 0;
        for (let i = 0; i < total; ++i) {
            const item = chipRepeater.itemAt(i);
            const step = (item ? item.implicitWidth : 0) + (fit > 0 ? Tokens.s2 : 0);
            if (used + step > budget)
                break;
            used += step;
            fit += 1;
        }
        // The +N chip needs a seat of its own; give back chips until it has one.
        if (fit < total) {
            while (fit > 0 && used + strip.overflowWidth + Tokens.s2 > budget) {
                const dropped = chipRepeater.itemAt(fit - 1);
                used -= (dropped ? dropped.implicitWidth : 0) + (fit > 1 ? Tokens.s2 : 0);
                fit -= 1;
            }
        }
        strip.shownCount = fit;
    }

    // Worst-case width of the "+N" pill, so the reserve never depends on how
    // many chips are currently hidden (which is what it is deciding).
    readonly property int overflowWidth: Math.ceil(overflowMetrics.advanceWidth) + Tokens.s7

    TextMetrics {
        id: overflowMetrics
        font.family: Tokens.sansFamily
        font.pixelSize: Tokens.textChip
        font.weight: Font.Medium
        text: "+" + strip.chipCount
    }

    Column {
        id: layout
        width: strip.width
        spacing: strip.narrow ? Tokens.s3 : 0

        Item {
            id: headLine
            width: parent.width
            height: Math.max(eyebrowLabel.implicitHeight,
                             strip.showEdit ? editButton.implicitHeight : 0,
                             strip.narrow ? 0 : chipsRow.implicitHeight)

            Eyebrow {
                id: eyebrowLabel
                text: qsTr("Binding")
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
            }

            // The chips' seat while they share the eyebrow's line.
            Item {
                id: inlineSlot
                anchors.left: eyebrowLabel.right
                anchors.leftMargin: Tokens.s5
                anchors.right: strip.showEdit ? editButton.left : parent.right
                anchors.rightMargin: Tokens.s5
                anchors.verticalCenter: parent.verticalCenter
                height: chipsRow.implicitHeight
            }

            DishButton {
                id: editButton
                visible: strip.showEdit
                text: qsTr("Edit ›")
                variant: DishButton.Outline
                size: DishButton.Small
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: qsTr("Configure binding")
                onClicked: strip.editRequested()
            }
        }

        // The chips' seat once they have their own line.
        Item {
            id: wrapSlot
            visible: strip.narrow
            width: parent.width
            height: strip.narrow ? chipsRow.implicitHeight : 0
        }
    }

    // One chip row, re-seated rather than duplicated: two Repeaters over the
    // same model would build every chip twice.
    Row {
        id: chipsRow
        parent: strip.narrow ? wrapSlot : inlineSlot
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: Tokens.s2

        Repeater {
            id: chipRepeater
            model: strip.chips

            delegate: CapabilityChip {
                id: chip
                required property int index
                required property var modelData

                // An invisible child is skipped by the Row, so the ones that did
                // not fit cost no width — but keep their implicit width, which
                // is what the measurement reads.
                visible: chip.index < strip.shownCount
                text: chip.modelData.text !== undefined ? chip.modelData.text : ""
                tone: chip.modelData.tone !== undefined ? chip.modelData.tone
                                                        : CapabilityChip.Neutral
            }
        }

        AbstractButton {
            id: overflowChip

            visible: strip.hiddenCount > 0
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            implicitWidth: overflowLabel.implicitWidth + Tokens.s7
            implicitHeight: overflowLabel.implicitHeight + Tokens.s2

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Show %n more binding settings", "", strip.hiddenCount)

            HoverHandler { cursorShape: Qt.PointingHandCursor }

            background: Item {
                Rectangle {
                    anchors.fill: parent
                    radius: Tokens.radiusChip
                    color: overflowChip.down ? Theme.primaryPress
                         : overflowChip.hovered ? Theme.primaryHover
                         : Theme.surfaceDim
                    border.width: 1
                    border.color: overflowChip.visualFocus ? Theme.primary : Theme.outline

                    Behavior on color {
                        enabled: !Tokens.reducedMotion
                        ColorAnimation { duration: Tokens.durFast }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: Tokens.radiusChip + 2
                    visible: overflowChip.visualFocus
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.focusRing
                }
            }

            contentItem: Text {
                id: overflowLabel
                text: "+" + strip.hiddenCount
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Tokens.textChip
                font.weight: Font.Medium
                color: Theme.mutedStrong
            }

            onClicked: {
                // Positioned at click time: mapToItem is not a reactive binding,
                // and the popup only needs to be right where it opens.
                const at = overflowChip.mapToItem(strip, 0, overflowChip.height + Tokens.s2);
                overflowPopup.x = Math.max(0, Math.min(at.x, strip.width - overflowPopup.width));
                overflowPopup.y = at.y;
                overflowPopup.open();
            }
        }
    }

    Popup {
        id: overflowPopup

        width: 320
        padding: Tokens.s5
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: Tokens.radiusCard
            color: Theme.surface
            border.width: 1
            border.color: Theme.outline
        }

        contentItem: Column {
            spacing: Tokens.s4

            Eyebrow {
                text: qsTr("Also in this binding")
                mutedTone: true
            }

            Repeater {
                model: strip.hiddenChips

                delegate: Column {
                    id: overflowRow
                    required property var modelData
                    width: overflowPopup.availableWidth
                    spacing: Tokens.s1

                    CapabilityChip {
                        text: overflowRow.modelData.text !== undefined
                              ? overflowRow.modelData.text : ""
                        tone: overflowRow.modelData.tone !== undefined
                              ? overflowRow.modelData.tone : CapabilityChip.Neutral
                    }
                    Text {
                        id: reasonText
                        visible: reasonText.text.length > 0
                        width: parent.width
                        text: overflowRow.modelData.reason !== undefined
                              ? overflowRow.modelData.reason : ""
                        wrapMode: Text.WordWrap
                        font.pixelSize: Tokens.textMeta
                        color: Theme.mutedStrong
                    }
                }
            }
        }
    }
}
