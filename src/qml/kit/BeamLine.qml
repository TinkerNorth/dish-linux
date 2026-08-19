// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// WireLine's vertical sibling: the satellite -> dish signal path in the
// installer's hero rail. A sibling rather than a rotation of WireLine because
// the label and embedded glyph are horizontal-only concerns — the rail owns its
// own glyphs, so this component is only the line.
//
// `live` claims a finished install (solid accent); `transmitting` keeps the
// dashes and crawls them along the beam — downward toward the dish while
// installing, upward while removing. The line must NOT read live until the
// work has actually succeeded.
//
// The dash is a Canvas (Qt 6.7 has no dashed-line primitive) and takes its
// colour through Qt.rgba(). NEVER String(Theme.outline): a QColor stringifies
// to #AARRGGBB and Canvas 2D parses that as #RRGGBBAA.

import QtQuick
import Dish.Chrome

Item {
    id: beam

    property bool live: false
    property bool transmitting: false
    // Crawl direction. false walks the dashes toward the beam's END (the dish
    // below); true walks them back toward the satellite (uninstall).
    property bool upward: false

    // Distance (px) into the 8px dash pattern, applied as the Canvas
    // lineDashOffset (negated for the downward crawl). Declared WITHOUT an
    // initialiser: that would be a binding, and a binding plus the
    // NumberAnimation value source below is a duplicate-property-binding error.
    property real dashPhase

    readonly property color lineColor: (beam.live || beam.transmitting) ? Theme.primary
                                                                        : Theme.outline

    implicitWidth: 2

    Canvas {
        id: dashCanvas
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const c = beam.lineColor;
            ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, c.a);
            ctx.lineWidth = 2;
            if (!beam.live)
                ctx.setLineDash([4, 4]);
            // The path runs top -> bottom, so a NEGATIVE offset walks the
            // pattern toward the path's end (down); positive walks it back up.
            ctx.lineDashOffset = beam.upward ? beam.dashPhase : -beam.dashPhase;
            const x = width / 2;
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, height);
            ctx.stroke();
        }

        Connections {
            target: Theme
            function onPaletteChanged() { dashCanvas.requestPaint(); }
        }
        Connections {
            target: beam
            function onLiveChanged() { dashCanvas.requestPaint(); }
            function onTransmittingChanged() { dashCanvas.requestPaint(); }
            function onUpwardChanged() { dashCanvas.requestPaint(); }
            function onDashPhaseChanged() { dashCanvas.requestPaint(); }
        }
    }

    NumberAnimation on dashPhase {
        running: beam.transmitting && !beam.live && beam.visible && !Tokens.reducedMotion
        loops: Animation.Infinite
        from: 0
        to: 8
        duration: Tokens.durBusy
    }

    onTransmittingChanged: {
        // Park on a phase boundary so a stopped beam never freezes mid-crawl.
        if (!beam.transmitting)
            beam.dashPhase = 0;
    }
}
