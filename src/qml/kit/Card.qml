// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The rounded, outlined panel. The one element that deliberately paints an
// opaque `Theme.surface`, so content reads against a panel and not the body.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

Control {
    id: card

    property bool filled: true
    property bool dense: false

    topPadding: card.dense ? Tokens.s4 : Tokens.s6
    bottomPadding: card.dense ? Tokens.s4 : Tokens.s6
    leftPadding: card.dense ? Tokens.s5 : Tokens.s7
    rightPadding: card.dense ? Tokens.s5 : Tokens.s7

    background: Rectangle {
        radius: Tokens.radiusCard
        color: card.filled ? Theme.surface : "transparent"
        border.width: 1
        border.color: Theme.outline
    }
}
