// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One stick on its gate: the ring is full travel, the dot is where the stick
// is. Takes each axis as a fraction, -1..1, with +y up as the wire carries it,
// so a page passes the inspector's numbers straight through. A pad that is not
// reporting draws no dot: a centred one would read as a stick at rest.

import QtQuick
import Dish.Chrome

Item {
    id: plot

    property real stickX: 0
    property real stickY: 0
    property bool reporting: true
    property string accessibleName: ""

    readonly property real gateRadius: Math.min(plot.width, plot.height) / 2
    readonly property real travel: plot.gateRadius - dot.width / 2
    readonly property real clampedX: Math.max(-1, Math.min(1, plot.stickX))
    readonly property real clampedY: Math.max(-1, Math.min(1, plot.stickY))

    implicitWidth: Tokens.glyphHero
    implicitHeight: Tokens.glyphHero

    Accessible.role: Accessible.Graphic
    Accessible.name: plot.accessibleName

    Rectangle {
        anchors.centerIn: parent
        width: plot.gateRadius * 2
        height: width
        radius: width / 2
        color: Theme.surfaceDim
        border.width: 1
        border.color: Theme.outline
    }

    Rectangle {
        anchors.centerIn: parent
        width: plot.gateRadius * 2
        height: 1
        color: Theme.outlineSubtle
    }

    Rectangle {
        anchors.centerIn: parent
        width: 1
        height: plot.gateRadius * 2
        color: Theme.outlineSubtle
    }

    Rectangle {
        id: dot
        visible: plot.reporting
        width: Tokens.dotSize + Tokens.s1
        height: width
        radius: width / 2
        color: Theme.primary
        x: plot.width / 2 + plot.clampedX * plot.travel - width / 2
        y: plot.height / 2 - plot.clampedY * plot.travel - height / 2
    }
}
