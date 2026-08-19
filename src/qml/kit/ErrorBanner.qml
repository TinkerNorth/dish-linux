// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The inline failure row. An error is a diagnosis AND a next step, never an
// apology — hence two strings:
//   text:   "Server unreachable"
//   detail: "Has it moved networks?"

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

Rectangle {
    id: banner

    enum Tone { Error, Warning }

    property string text: ""
    property string detail: ""
    // Error is a failure; Warning is "live but faltering".
    property int tone: ErrorBanner.Error
    property string retryText: qsTr("Retry")
    property bool showRetry: false

    signal retryRequested()

    readonly property color toneColor: banner.tone === ErrorBanner.Warning
                                       ? Theme.warning : Theme.error

    visible: banner.text.length > 0

    implicitHeight: layout.implicitHeight + 2 * Tokens.s6
    implicitWidth: layout.implicitWidth + 2 * Tokens.s7
    radius: Tokens.radiusCard
    color: banner.tone === ErrorBanner.Warning ? Theme.warningFill : Theme.errorFill
    border.width: 1
    border.color: Theme.alpha(banner.toneColor, 0.35)

    Accessible.role: Accessible.AlertMessage
    Accessible.name: banner.text
    Accessible.description: banner.detail

    RowLayout {
        id: layout
        anchors.fill: parent
        anchors.leftMargin: Tokens.s7
        anchors.rightMargin: Tokens.s7
        anchors.topMargin: Tokens.s6
        anchors.bottomMargin: Tokens.s6
        spacing: Tokens.s5

        StatusDot {
            token: banner.tone === ErrorBanner.Warning ? "warning" : "error"
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: Tokens.s2
        }

        ColumnLayout {
            spacing: Tokens.s1
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            Label {
                text: banner.text
                color: Theme.onSurface
                font.pixelSize: Tokens.textBase
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: banner.detail
                visible: banner.detail.length > 0
                color: Theme.mutedStrong
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        DishButton {
            text: banner.retryText
            visible: banner.showRetry
            variant: DishButton.Outline
            size: DishButton.Small
            Layout.alignment: Qt.AlignVCenter
            onClicked: banner.retryRequested()
        }
    }
}
