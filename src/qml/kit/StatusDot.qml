// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Maps a contract dot token onto a Theme colour. 10px with a ring, not 8px
// flat: at 8px the success and warning hues are a coin-flip for a
// low-vision user, and the ring gives the dot an edge independent of its fill.

import QtQuick
import Dish.Chrome

Rectangle {
    id: dot

    property string token: "muted"

    readonly property color tone: dot.token === "success" ? Theme.success
                                : dot.token === "warning" ? Theme.warning
                                : dot.token === "primary" ? Theme.primary
                                : dot.token === "error" ? Theme.error
                                : Theme.muted

    implicitWidth: 10
    implicitHeight: 10
    radius: width / 2

    // The chip beside it carries the word; announcing both reads every status
    // twice.
    Accessible.ignored: true

    color: dot.tone
    border.width: 1
    border.color: Qt.darker(dot.tone, 1.4)
}
