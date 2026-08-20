// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one button type. States (hover, press, focus, disabled) are never
// variants — `enabled: false` is what UI Automation and Narrator report.

import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome

Button {
    id: control

    enum Variant { Primary, Outline, Destructive }
    enum Size { Normal, Small, Large }

    property int variant: DishButton.Outline
    property int size: DishButton.Normal

    readonly property bool compact: control.size === DishButton.Small
    // The installer's verb: one per face, fill-width at the use site.
    readonly property bool large: control.size === DishButton.Large

    // The label colour, and for the outlined variants the border colour too.
    readonly property color foreground: !control.enabled ? Theme.disabledFg
                                      : control.variant === DishButton.Primary ? Theme.onPrimary
                                      : control.variant === DishButton.Destructive ? Theme.error
                                      : Theme.primary

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    font.pixelSize: control.large ? 15 : control.compact ? Tokens.textMeta : Tokens.textBase
    font.weight: control.large ? Font.DemiBold : Font.Medium
    implicitHeight: control.large ? 46 : control.compact ? 24 : 30
    leftPadding: control.compact ? Tokens.s5 : Tokens.s6
    rightPadding: control.compact ? Tokens.s5 : Tokens.s6

    // The corner tracks the height: the 46px verb rounds like a card, not
    // like a chip.
    readonly property int cornerRadius: control.large ? Tokens.radiusCard : Tokens.radiusButton

    opacity: control.enabled ? 1.0 : Tokens.disabledOpacity

    Accessible.role: Accessible.Button
    Accessible.name: control.text

    // Enter activates a focused button (desktop convention; the Basic style
    // only wires Space). clicked() is emitted directly — there is no
    // transient press state to animate for a key activation.
    Keys.onReturnPressed: control.clicked()
    Keys.onEnterPressed: control.clicked()

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.foreground
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: control.cornerRadius
            // The outlined pair wash over a transparent ground so the body still
            // reads through a button placed on bare chrome.
            color: control.variant === DishButton.Primary
                     ? (!control.enabled ? Theme.surfaceDim
                        : control.down || control.hovered ? Theme.primaryDark
                        : Theme.primary)
                 : control.variant === DishButton.Destructive
                     ? (control.down ? Theme.alpha(Theme.error, 0.18)
                        : control.hovered ? Theme.alpha(Theme.error, 0.12)
                        : "transparent")
                     : (control.down ? Theme.primaryPress
                        : control.hovered ? Theme.primaryHover
                        : "transparent")
            border.width: control.variant === DishButton.Primary
                          ? (control.visualFocus ? 1 : 0) : 1
            border.color: control.variant === DishButton.Primary
                          ? Theme.primary : control.foreground

            Behavior on color {
                enabled: !Tokens.reducedMotion
                ColorAnimation { duration: Tokens.durFast }
            }
        }

        // visualFocus, not focus: a mouse press must never ring.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: control.cornerRadius + 2
            visible: control.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
        }
    }
}
