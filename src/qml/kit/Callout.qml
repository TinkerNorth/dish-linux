// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Advice attached to the step the user is on; ErrorBanner is for something that
// failed and can be retried.
//
// The default property is redirected to the trailing action row, so everything
// declared here goes through an explicit property (`background` /
// `contentItem`) — a bare child would re-parent into that row, inside itself.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

Control {
    id: callout

    enum Tone { Info, Warning, Error }

    property int tone: Callout.Info
    property string text: ""
    property string glyph: ""

    default property alias actions: actionRow.data

    readonly property color toneColor: callout.tone === Callout.Warning ? Theme.warning
                                     : callout.tone === Callout.Error ? Theme.error
                                     : Theme.primary

    topPadding: Tokens.s5
    bottomPadding: Tokens.s5
    leftPadding: Tokens.s6
    rightPadding: Tokens.s6

    Accessible.role: Accessible.StaticText
    Accessible.name: callout.text

    background: Rectangle {
        radius: Tokens.radiusButton
        color: callout.tone === Callout.Warning ? Theme.warningFill
             : callout.tone === Callout.Error ? Theme.errorFill
             : Theme.primaryFill
    }

    contentItem: RowLayout {
        spacing: Tokens.s5

        BrandGlyph {
            glyph: callout.glyph
            visible: callout.glyph.length > 0
            Layout.preferredWidth: Tokens.glyphSm
            Layout.preferredHeight: Tokens.glyphSm
            Layout.alignment: Qt.AlignTop
        }

        Text {
            text: callout.text
            color: callout.toneColor
            font.pixelSize: Tokens.textMeta
            lineHeight: 1.5
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
        }

        Row {
            id: actionRow
            spacing: Tokens.s4
            visible: actionRow.implicitWidth > 0
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
