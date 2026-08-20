// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The panel behind the update pill. It NEVER opens by itself and never takes
// focus: an update is an offer, not an interruption, and the app must stay
// usable with it on screen. Every action here is also reachable from Settings,
// so dismissing it costs the user nothing.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "kit" as Kit

Popup {
    id: popover

    width: 320
    padding: Tokens.s7

    modal: false
    dim: false
    focus: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: Tokens.radiusDialog
        color: Theme.surface
        border.width: 1
        border.color: Theme.outline
    }

    contentItem: ColumnLayout {
        spacing: Tokens.s4

        Label {
            Layout.fillWidth: true
            text: qsTr("Update available")
            color: Theme.onSurface
            font.pixelSize: Tokens.textHeading
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
        }

        // The offer's one fact, in the mono voice the app uses for every
        // machine reading.
        Label {
            Layout.fillWidth: true
            text: qsTr("Dish %1").arg(App.updateVersion)
            color: Theme.muted
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textMeta
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Dish is installed by your package manager, so it updates itself there. The release page has the packages and the notes.")
            color: Theme.muted
            font.pixelSize: Tokens.textSummary
            wrapMode: Text.WordWrap
        }

        Kit.Callout {
            Layout.fillWidth: true
            visible: App.updateRequired
            tone: Kit.Callout.Warning
            text: qsTr("This version is no longer supported.")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s2
            spacing: Tokens.s4

            Kit.KitButton {
                text: qsTr("Open release page")
                onClicked: {
                    popover.close();
                    App.openReleaseNotes();
                }
            }

            Item { Layout.fillWidth: true }
        }

        // Hidden while the update is required: there is nothing to skip to.
        Kit.OutlineButton {
            Layout.alignment: Qt.AlignLeft
            visible: !App.updateRequired
            size: Kit.DishButton.Small
            text: qsTr("Skip this version")
            onClicked: {
                popover.close();
                App.skipUpdate();
            }
        }
    }
}
