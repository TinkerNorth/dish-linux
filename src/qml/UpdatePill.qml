// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The updater's one piece of chrome: a header control that exists only while
// there is something to say. Disabled, idle, up-to-date, checking and failed
// render NOTHING — a check is silent by requirement and a failure is a Settings
// matter — so a build that never sees an update is visually identical to one
// with no updater at all.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome
import "kit" as Kit

AbstractButton {
    id: pill

    visible: App.updatePhase === "available"

    // Canvas 2D parses a stringified colour as #RRGGBBAA, so only an OPAQUE
    // Theme role may be handed to it this way — muted and primary are opaque by
    // construction (kit rule C5).
    readonly property color glyphColor: App.updateRequired ? Theme.warning : Theme.primary

    readonly property string stateText: qsTr("Update available: Dish %1").arg(App.updateVersion)

    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.Button
    Accessible.name: pill.stateText

    onClicked: popover.opened ? popover.close() : popover.open()

    onGlyphColorChanged: glyphCanvas.requestPaint()

    background: Rectangle {
        radius: Tokens.radiusChip
        color: pill.hovered ? Theme.primaryHover : "transparent"
        border.width: pill.visualFocus ? 1 : 0
        border.color: Theme.primary

        Rectangle {
            visible: pill.visualFocus
            anchors.fill: parent
            anchors.margins: -Tokens.s1
            radius: Tokens.radiusChip + Tokens.s1
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }

    contentItem: Item {
        id: glyphCell

        // An arrow leaving a tray: Dish points at the release, it never
        // installs one.
        Canvas {
            id: glyphCanvas
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                ctx.strokeStyle = String(pill.glyphColor);
                ctx.lineWidth = 1.4;
                ctx.lineCap = "round";
                ctx.lineJoin = "round";
                var cx = width / 2, cy = height / 2;
                ctx.beginPath();
                ctx.moveTo(cx, cy + 1);
                ctx.lineTo(cx, cy - 6);
                ctx.moveTo(cx - 3.5, cy - 2.5);
                ctx.lineTo(cx, cy - 6);
                ctx.lineTo(cx + 3.5, cy - 2.5);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(cx - 5, cy + 2.5);
                ctx.lineTo(cx - 5, cy + 5);
                ctx.lineTo(cx + 5, cy + 5);
                ctx.lineTo(cx + 5, cy + 2.5);
                ctx.stroke();
            }
            Connections {
                target: Theme
                function onPaletteChanged() { glyphCanvas.requestPaint(); }
            }
        }

        // The one arrival moment: a single rise plus one scale pulse when an
        // update first appears, then static forever. Never loops.
        SequentialAnimation {
            id: arriveAnimation
            running: false
            NumberAnimation {
                target: glyphCell
                property: "y"
                from: Tokens.s2
                to: 0
                duration: Tokens.durFast
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                target: glyphCell
                property: "scale"
                from: 1.0
                to: 1.12
                duration: Tokens.durFast
                easing.type: Easing.OutQuad
            }
            NumberAnimation {
                target: glyphCell
                property: "scale"
                from: 1.12
                to: 1.0
                duration: Tokens.durNormal
                easing.type: Easing.OutBack
            }
        }
    }

    onVisibleChanged: {
        if (pill.visible && !Tokens.reducedMotion)
            arriveAnimation.restart();
    }

    // 7px, smaller than the 10px StatusDot default: this is a badge on a glyph,
    // not a status row's dot. Warning tone when the running build is no longer
    // supported.
    Kit.StatusDot {
        visible: App.updateRequired
        token: "warning"
        width: 7
        height: 7
        x: pill.width / 2 + Tokens.s3
        y: pill.height / 2 - Tokens.s5
    }

    // Declared, never attached — see DishToolTip in QML_UI_KIT.md.
    Kit.DishToolTip {
        id: pillTip
        visible: pill.hovered && !popover.opened
        delay: 500
        text: pill.stateText
        y: pill.height + Tokens.s2
    }

    UpdatePopover {
        id: popover
        y: pill.height + Tokens.s1
        x: pill.width - width
    }
}
