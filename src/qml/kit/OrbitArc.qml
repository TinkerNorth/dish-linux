// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A static dashed elliptical arc: the orbit hint behind the hero rail's
// satellite. Deliberately NEVER animated — decorative motion is banned
// brand-wide; the orbit is scenery, the beam is the story. The dash treatment
// is the BannerSlot dashed-edge language ([3,3], outlineSubtle).
//
// Canvas because Qt 6.7 has no dashed-ellipse primitive; colour goes through
// Qt.rgba() per kit rule C5 (a stringified QColor is #AARRGGBB and Canvas
// parses #RRGGBBAA).

import QtQuick
import Dish.Chrome

Item {
    id: arc

    // Ellipse radii in px; the arc is centred in this item.
    property real rx: 72
    property real ry: 26
    // Degrees of arc drawn, from the ellipse's rightmost point counter-
    // clockwise. 360 = the full orbit.
    property real sweep: 360

    implicitWidth: arc.rx * 2 + 2
    implicitHeight: arc.ry * 2 + 2

    Canvas {
        id: orbitCanvas
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const c = Theme.outlineSubtle;
            ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, c.a);
            ctx.lineWidth = 1;
            ctx.setLineDash([3, 3]);
            // Scaled circle rather than ellipse(): stroke AFTER the transform
            // is restored so the 1px line width is not squashed with the path.
            ctx.save();
            ctx.translate(width / 2, height / 2);
            ctx.scale(1, arc.ry / Math.max(1, arc.rx));
            ctx.beginPath();
            ctx.arc(0, 0, arc.rx, 0, (Math.PI / 180) * arc.sweep);
            ctx.restore();
            ctx.stroke();
        }

        Connections {
            target: Theme
            function onPaletteChanged() { orbitCanvas.requestPaint(); }
        }
        Connections {
            target: arc
            function onRxChanged() { orbitCanvas.requestPaint(); }
            function onRyChanged() { orbitCanvas.requestPaint(); }
            function onSweepChanged() { orbitCanvas.requestPaint(); }
        }
    }
}
