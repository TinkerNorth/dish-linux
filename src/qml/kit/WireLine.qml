// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pad -> host wire. `live` is the only state that claims a working link;
// `transmitting` keeps the dashes and crawls them toward the host, because the
// wire must NOT read live until the bind has actually succeeded.
//
// The dash is a Canvas (Qt 6.7 has no dashed-line primitive) and takes its
// colour through Qt.rgba(). NEVER String(Theme.outline): a QColor stringifies
// to #AARRGGBB and Canvas 2D parses that as #RRGGBBAA.

import QtQuick
import Dish.Chrome

Item {
    id: wire

    property bool live: false
    property string label: ""
    property bool showGlyph: true
    property bool transmitting: false

    // Distance (px) into the 8px dash pattern, read by the Canvas as a NEGATIVE
    // lineDashOffset so the pattern walks toward the host. Declared WITHOUT an
    // initialiser: that would be a binding, and a binding plus the
    // NumberAnimation value source below is a duplicate-property-binding error.
    property real dashPhase

    readonly property color lineColor: (wire.live || wire.transmitting) ? Theme.primary
                                                                        : Theme.outline

    implicitWidth: 60
    implicitHeight: stack.implicitHeight

    Column {
        id: stack
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Tokens.s2

        Text {
            id: caption
            visible: wire.label.length > 0
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: wire.label
            elide: Text.ElideRight
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textChip
            color: (wire.live || wire.transmitting) ? Theme.primary : Theme.mutedStrong
        }

        Item {
            id: line
            width: parent.width
            height: wire.showGlyph ? Tokens.glyphMd : 2

            Canvas {
                id: dashCanvas
                anchors.fill: parent

                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    const c = wire.lineColor;
                    ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, c.a);
                    ctx.lineWidth = 2;
                    if (!wire.live)
                        ctx.setLineDash([4, 4]);
                    ctx.lineDashOffset = -wire.dashPhase;
                    const y = height / 2;
                    const gap = wire.showGlyph ? Tokens.glyphMd / 2 + Tokens.s4 : 0;
                    ctx.beginPath();
                    ctx.moveTo(0, y);
                    ctx.lineTo(Math.max(0, width / 2 - gap), y);
                    ctx.moveTo(Math.min(width, width / 2 + gap), y);
                    ctx.lineTo(width, y);
                    ctx.stroke();
                }

                Connections {
                    target: Theme
                    function onPaletteChanged() { dashCanvas.requestPaint(); }
                }
                Connections {
                    target: wire
                    function onLiveChanged() { dashCanvas.requestPaint(); }
                    function onTransmittingChanged() { dashCanvas.requestPaint(); }
                    function onShowGlyphChanged() { dashCanvas.requestPaint(); }
                    function onDashPhaseChanged() { dashCanvas.requestPaint(); }
                }
            }

            BrandGlyph {
                id: dish
                visible: wire.showGlyph
                anchors.centerIn: parent
                width: Tokens.glyphMd
                height: Tokens.glyphMd
                glyph: wire.live ? "dish-connected" : wire.transmitting ? "dish" : "dish-off"

                // The one sanctioned transient for a brand glyph: an opacity
                // pulse over the BASE asset, never a `*-animated` file (those
                // ship no <animate> and Qt runs no SMIL).
                SequentialAnimation on opacity {
                    running: wire.transmitting && !wire.live && wire.visible
                             && !Tokens.reducedMotion
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { to: 0.45; duration: Tokens.durBusy / 2 }
                    NumberAnimation { to: 1.0; duration: Tokens.durBusy / 2 }
                }
            }
        }
    }

    NumberAnimation on dashPhase {
        running: wire.transmitting && !wire.live && wire.visible && !Tokens.reducedMotion
        loops: Animation.Infinite
        from: 0
        to: 8
        duration: Tokens.durBusy
    }

    onTransmittingChanged: {
        // Park on a phase boundary so a stopped wire never freezes mid-crawl.
        if (!wire.transmitting)
            wire.dashPhase = 0;
    }
}
