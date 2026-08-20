// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure indicator: the enclosing row owns the click surface, focus ring and
// keyboard model, and already announces itself as a checked RadioButton — hence
// Accessible.ignored, or every list reads twice.

import QtQuick
import Dish.Chrome

Rectangle {
    id: mark

    property bool selected: false

    width: 15
    height: 15
    radius: width / 2
    color: "transparent"
    border.width: 1.5
    border.color: !mark.enabled ? Theme.disabledFg
                : mark.selected ? Theme.primary
                : Theme.outline

    Accessible.ignored: true

    Rectangle {
        anchors.centerIn: parent
        width: 7
        height: 7
        radius: width / 2
        color: mark.enabled ? Theme.primary : Theme.disabledFg
        visible: mark.selected
    }
}
