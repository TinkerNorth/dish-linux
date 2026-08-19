// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The dashed invitation control ("+ Add a controller"), plus `placeholder`: the
// same dashed shape with no action in it, which is information rather than a
// dead control and so never dims.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

AbstractButton {
    id: card

    property string title: ""
    property string subtitle: ""
    property bool showPlus: false
    property bool placeholder: false

    focusPolicy: card.placeholder ? Qt.NoFocus : Qt.StrongFocus
    hoverEnabled: !card.placeholder

    topPadding: Tokens.s5
    bottomPadding: Tokens.s5
    leftPadding: Tokens.s6
    rightPadding: Tokens.s6

    opacity: (card.placeholder || card.enabled) ? 1.0 : Tokens.disabledOpacity

    Accessible.role: card.placeholder ? Accessible.StaticText : Accessible.Button
    Accessible.name: card.title
    Accessible.description: card.subtitle

    HoverHandler {
        enabled: !card.placeholder
        cursorShape: Qt.PointingHandCursor
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: Tokens.radiusCard + 2
            visible: card.visualFocus && !card.placeholder
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }

        Rectangle {
            anchors.fill: parent
            radius: Tokens.radiusCard
            color: card.placeholder ? "transparent"
                 : card.down ? Theme.accentWash24
                 : card.hovered ? Theme.primaryPress
                 : Theme.primaryFill

            Behavior on color {
                enabled: !Tokens.reducedMotion
                ColorAnimation { duration: Tokens.durFast }
            }
        }

        // Canvas because Qt 6.7 Shapes has no rounded dashed rect primitive.
        Canvas {
            id: borderCanvas
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                var c = card.placeholder ? Theme.outline
                      : card.enabled ? Theme.primary : Theme.disabledFg;
                ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, c.a);
                ctx.lineWidth = 1;
                if (!card.visualFocus)
                    ctx.setLineDash([3, 3]);
                ctx.beginPath();
                ctx.roundedRect(0.5, 0.5, width - 1, height - 1,
                                Tokens.radiusCard, Tokens.radiusCard);
                ctx.stroke();
            }
            Connections {
                target: Theme
                function onPaletteChanged() { borderCanvas.requestPaint(); }
            }
            Connections {
                target: card
                function onVisualFocusChanged() { borderCanvas.requestPaint(); }
                function onPlaceholderChanged() { borderCanvas.requestPaint(); }
                function onEnabledChanged() { borderCanvas.requestPaint(); }
            }
        }
    }

    contentItem: Row {
        spacing: Tokens.s5

        Text {
            visible: card.showPlus
            text: "+"
            color: card.placeholder ? Theme.mutedStrong
                 : card.enabled ? Theme.primary : Theme.disabledFg
            font.pixelSize: Tokens.textHeading
            anchors.verticalCenter: parent.verticalCenter
        }
        Column {
            spacing: Tokens.s1
            anchors.verticalCenter: parent.verticalCenter

            Text {
                text: card.title
                color: card.placeholder ? Theme.mutedStrong
                     : card.enabled ? Theme.primary : Theme.disabledFg
                font.pixelSize: Tokens.textBase
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                width: Math.min(implicitWidth,
                                card.availableWidth
                                - (card.showPlus ? Tokens.textHeading + Tokens.s5 : 0))
            }
            Text {
                visible: card.subtitle.length > 0
                text: card.subtitle
                color: Theme.muted
                font.pixelSize: Tokens.textMeta
                elide: Text.ElideRight
                width: Math.min(implicitWidth,
                                card.availableWidth
                                - (card.showPlus ? Tokens.textHeading + Tokens.s5 : 0))
            }
        }
    }
}
