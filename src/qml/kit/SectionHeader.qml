// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Callers pass natural-cased `label`; the casing is a FONT property, not a
// string transform — .toUpperCase() in QML uses the C locale and would mangle
// e.g. Turkish dotless i.

import QtQuick
import Dish.Chrome

Row {
    id: header

    property string label: ""
    property string glyph: ""

    spacing: Tokens.s3

    BrandGlyph {
        glyph: header.glyph
        width: Tokens.glyphSm
        height: Tokens.glyphSm
        visible: header.glyph.length > 0
        anchors.verticalCenter: parent.verticalCenter
    }

    Text {
        text: header.label
        color: Theme.primary
        font.family: Tokens.monoFamily
        font.pixelSize: Tokens.textMeta
        font.weight: Font.DemiBold
        font.letterSpacing: Tokens.sectionLetterSpacing
        font.capitalization: Font.AllUppercase
        anchors.verticalCenter: parent.verticalCenter
    }
}
