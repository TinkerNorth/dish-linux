// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The two-up choice card. Same selection rule as SelectRow; the difference is
// shape, not semantics — a card gives the body room to explain the trade-off.
// ←/→ move AND select among sibling cards.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

AbstractButton {
    id: control

    property bool selected: false
    property string title: ""
    property string body: ""
    property string badgeText: ""
    property int badgeTone: CapabilityChip.Ok

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    topPadding: Tokens.s4
    bottomPadding: Tokens.s4
    leftPadding: Tokens.s5
    rightPadding: Tokens.s5

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.RadioButton
    Accessible.checked: control.selected
    Accessible.name: control.title
                     + (control.badgeText.length > 0 ? " · " + control.badgeText : "")
    Accessible.description: control.body

    Keys.onLeftPressed: control.stepSelection(false)
    Keys.onRightPressed: control.stepSelection(true)

    HoverHandler { cursorShape: Qt.PointingHandCursor }

    // Clamped at both ends; the sibling test is structural (same parent,
    // exposes `selected`) so it never escapes the group into the footer.
    function stepSelection(forward) {
        if (!control.enabled)
            return;
        // Indexed through variables, not `item.selected`: the focus chain is
        // typed as a bare Item, so a dotted access would not resolve statically.
        const flag = "selected";
        const sig = "clicked";
        let item = control;
        for (let guard = 0; guard < 64; ++guard) {
            item = item.nextItemInFocusChain(forward);
            if (!item || item === control)
                return;
            if (item.parent === control.parent && typeof item[flag] === "boolean") {
                item.forceActiveFocus(Qt.TabFocusReason);
                item[sig]();
                return;
            }
        }
    }

    background: Item {
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

    contentItem: ColumnLayout {
        spacing: Tokens.s1

        RowLayout {
            spacing: Tokens.s4
            Layout.fillWidth: true

            Text {
                text: control.title
                color: !control.enabled ? Theme.disabledFg
                     : control.selected ? Theme.primary : Theme.onSurface
                font.pixelSize: Tokens.textSummary
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            CapabilityChip {
                text: control.badgeText
                tone: control.badgeTone
                visible: control.badgeText.length > 0
                Layout.alignment: Qt.AlignVCenter
            }
        }

        Text {
            text: control.body
            visible: control.body.length > 0
            color: Theme.mutedStrong
            font.pixelSize: Tokens.textMeta
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
