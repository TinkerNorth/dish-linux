// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Every list has one. `body` always names a next step — never a bare spinner,
// never a bare "none". Goes inside a Card, so it reads against a surface.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

ColumnLayout {
    id: empty

    property string title: ""
    property string body: ""
    property string actionText: ""
    property bool showAction: false
    property string glyph: ""

    signal actionRequested()

    spacing: Tokens.s5

    BrandGlyph {
        glyph: empty.glyph
        Layout.preferredWidth: Tokens.glyphXl
        Layout.preferredHeight: Tokens.glyphXl
        visible: empty.glyph.length > 0
        Layout.alignment: Qt.AlignHCenter
    }

    Label {
        text: empty.title
        visible: empty.title.length > 0
        color: Theme.onSurface
        font.pixelSize: Tokens.textBase
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignHCenter
    }

    Label {
        text: empty.body
        visible: empty.body.length > 0
        // Information the user must read: a colour, never an opacity.
        color: Theme.mutedStrong
        font.pixelSize: Tokens.textSummary
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.maximumWidth: 400
        Layout.alignment: Qt.AlignHCenter
    }

    DishButton {
        text: empty.actionText
        visible: empty.showAction && empty.actionText.length > 0
        variant: DishButton.Outline
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: Tokens.s3
        onClicked: empty.actionRequested()
    }
}
