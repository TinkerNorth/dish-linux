// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The modal-overlay base for tasks that interrupt the shell. No shadow: the
// scrim carries modality, and a blurred layer per dialog costs a composited
// pass on a window that already paints an opaque body.
//
// Accept does NOT auto-close — an accept that fails (a rejected PIN) must keep
// its surface open, so the caller decides.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

Popup {
    id: dialog

    property string eyebrow: ""
    property string heading: ""
    // Empty accept/reject text hides that button.
    property string acceptText: qsTr("OK")
    property string rejectText: qsTr("Cancel")
    property bool acceptEnabled: true
    property bool destructiveAccept: false
    property int preferredWidth: 430
    // Aliased to the column's `data` so the property carries a concrete list
    // type: an object alias would hide the target's members from qmllint and
    // force every call site to suppress missing-property.
    property alias body: bodyColumn.data

    signal accepted()
    signal rejected()

    modal: true
    dim: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(preferredWidth, (parent ? parent.width : preferredWidth) - 48)
    padding: 0
    closePolicy: Popup.CloseOnEscape

    // Palette-derived, so a theme flip re-tints the scrim too.
    Overlay.modal: Rectangle {
        color: Theme.scrim
    }

    background: Rectangle {
        radius: Tokens.radiusDialog
        color: Theme.surface
        border.width: 1
        border.color: Theme.outline
    }

    contentItem: ColumnLayout {
        spacing: Tokens.s6

        Eyebrow {
            text: dialog.eyebrow
            visible: dialog.eyebrow.length > 0
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s9
            Layout.leftMargin: Tokens.s9
            Layout.rightMargin: Tokens.s9
        }

        Label {
            text: dialog.heading
            visible: dialog.heading.length > 0
            color: Theme.onSurface
            font.pixelSize: Tokens.textHeading
            font.bold: true
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            // The eyebrow, when present, owns the top inset.
            Layout.topMargin: dialog.eyebrow.length > 0 ? -Tokens.s2 : Tokens.s9
            Layout.leftMargin: Tokens.s9
            Layout.rightMargin: Tokens.s9
        }

        ColumnLayout {
            id: bodyColumn
            spacing: Tokens.s6
            Layout.fillWidth: true
            Layout.leftMargin: Tokens.s9
            Layout.rightMargin: Tokens.s9
        }

        RowLayout {
            spacing: Tokens.s4
            Layout.alignment: Qt.AlignRight
            Layout.bottomMargin: Tokens.s9
            Layout.leftMargin: Tokens.s9
            Layout.rightMargin: Tokens.s9

            DishButton {
                text: dialog.rejectText
                visible: dialog.rejectText.length > 0
                variant: DishButton.Outline
                onClicked: { dialog.rejected(); dialog.close(); }
            }
            DishButton {
                text: dialog.acceptText
                visible: dialog.acceptText.length > 0
                enabled: dialog.acceptEnabled
                variant: dialog.destructiveAccept ? DishButton.Destructive
                                                  : DishButton.Primary
                onClicked: dialog.accepted()   // page decides whether to close
            }
        }
    }
}
