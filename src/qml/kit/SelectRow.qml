// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one selectable row. Selection is 1px Theme.primary + Theme.primaryFill,
// never 2px: the only 2px border in the app is the capture-armed card, which is
// an armed state, not a selection.
//
// The row does not own the group — it reports `picked()` and renders whatever
// `selected` the caller binds. Up/Down move AND select among sibling rows.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

AbstractButton {
    id: control

    property bool selected: false
    property string title: ""
    property string subtitle: ""
    property string glyph: ""
    property string dotToken: ""
    property string chipText: ""
    property int chipTone: CapabilityChip.Neutral

    // Right-hand slot: chips, badges, anything the row carries after its copy.
    default property alias extra: extraSlot.data

    // The caller applies the choice; the row never writes `selected` itself.
    signal picked()

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    topPadding: Tokens.s3
    bottomPadding: Tokens.s3
    leftPadding: Tokens.s5
    rightPadding: Tokens.s5

    implicitHeight: Math.max(Tokens.hitRow,
                             control.implicitContentHeight
                             + control.topPadding + control.bottomPadding)

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.RadioButton
    Accessible.checked: control.selected
    Accessible.name: control.title
                     + (control.subtitle.length > 0 ? " · " + control.subtitle : "")
                     + (control.chipText.length > 0 ? " · " + control.chipText : "")

    onClicked: control.picked()

    Keys.onUpPressed: control.stepSelection(false)
    Keys.onDownPressed: control.stepSelection(true)

    // Clamped at both ends; the sibling test is structural (same parent,
    // exposes picked()) so it works for a ListView delegate and a Column child
    // alike.
    function stepSelection(forward) {
        if (!control.enabled)
            return;
        // Indexed through a variable, not `item.picked`: the focus chain is
        // typed as a bare Item, so a dotted access would not resolve statically.
        const sig = "picked";
        let item = control;
        for (let guard = 0; guard < 64; ++guard) {
            item = item.nextItemInFocusChain(forward);
            if (!item || item === control)
                return;
            if (item.parent === control.parent && typeof item[sig] === "function") {
                item.forceActiveFocus(Qt.TabFocusReason);
                item[sig]();
                return;
            }
        }
    }

    // The default property is redirected to `extra`, so this file's own objects
    // all go through an explicit property: a bare child (the cursor handler
    // included) would re-parent into that slot and merely cover it.
    background: Item {
        HoverHandler { cursorShape: Qt.PointingHandCursor }

        Rectangle {
            anchors.fill: parent
            radius: Tokens.radiusCard
            color: control.selected
                     ? (control.down ? Theme.accentWash24
                        : control.hovered ? Theme.primaryPress
                        : Theme.primaryFill)
                     : (control.down ? Theme.primaryPress
                        : control.hovered ? Theme.primaryHover
                        : "transparent")
            border.width: 1
            border.color: !control.enabled ? Theme.disabledFg
                        : control.selected ? Theme.primary
                        : Theme.outline

            Behavior on color {
                enabled: !Tokens.reducedMotion
                ColorAnimation { duration: Tokens.durFast }
            }
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: Tokens.radiusCard + 2
            visible: control.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }

    contentItem: RowLayout {
        spacing: Tokens.s5

        RadioMark {
            selected: control.selected
            enabled: control.enabled
            Layout.alignment: Qt.AlignVCenter
        }

        BrandGlyph {
            glyph: control.glyph
            visible: control.glyph.length > 0
            Layout.preferredWidth: Tokens.glyphMd
            Layout.preferredHeight: Tokens.glyphMd
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            spacing: Tokens.s1
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter

            Text {
                text: control.title
                color: !control.enabled ? Theme.disabledFg
                     : control.selected ? Theme.primary : Theme.onSurface
                font.pixelSize: Tokens.textSummary
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Text {
                text: control.subtitle
                visible: control.subtitle.length > 0
                color: Theme.mutedStrong
                font.pixelSize: Tokens.textMeta
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        // No explicit width, so this reports its natural extent and the copy
        // column (fillWidth + elide) gives way first at a narrow window.
        Item {
            id: extraSlot
            implicitWidth: extraSlot.childrenRect.width
            implicitHeight: extraSlot.childrenRect.height
            visible: extraSlot.implicitWidth > 0
            Layout.alignment: Qt.AlignVCenter
        }

        StatusDot {
            token: control.dotToken
            visible: control.dotToken.length > 0
            Layout.alignment: Qt.AlignVCenter
        }

        CapabilityChip {
            text: control.chipText
            tone: control.chipTone
            visible: control.chipText.length > 0
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
