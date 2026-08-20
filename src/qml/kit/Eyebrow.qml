// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The micro-label above dialog headings and hero titles; SectionHeader is the
// structural one. Callers pass natural case: uppercasing is a font property,
// never a string transform, because an uppercased literal cannot be translated.

import QtQuick
import Dish.Chrome

Text {
    property bool mutedTone: false

    font.family: Tokens.monoFamily
    font.pixelSize: Tokens.textChip
    font.weight: Font.DemiBold
    font.letterSpacing: Tokens.sectionLetterSpacing
    font.capitalization: Font.AllUppercase
    color: mutedTone ? Theme.muted : Theme.primary
}
