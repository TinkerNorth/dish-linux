// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The two terminal blockers only. An unsteady link is an inline ErrorBanner:
// a flapping link would raise and dismiss a modal the user cannot outrun.
//
// No retry countdown: reconnect backoff lives in the session state machine and
// is not wired to the live connection manager, so any number here is fiction.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

ContentDialog {
    id: blocker

    enum Kind {
        ConnectionLost,
        ControllerUnplugged
    }

    property int kind: BlockerDialog.ConnectionLost
    property string hostName: ""

    signal reconnectRequested()

    readonly property bool hostLost: blocker.kind === BlockerDialog.ConnectionLost

    heading: blocker.hostLost ? qsTr("Connection lost") : qsTr("Controller unplugged")

    rejectText: qsTr("Cancel")
    // An unplugged pad is fixed with a hand, not a button.
    acceptText: blocker.hostLost ? qsTr("Reconnect") : ""

    onAccepted: blocker.reconnectRequested()

    body: [
        RowLayout {
            spacing: Tokens.s5
            Layout.fillWidth: true

            StatusDot {
                token: blocker.hostLost ? "error" : "warning"
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: Tokens.s2
            }

            Label {
                text: blocker.hostLost
                      ? qsTr("%1 stopped responding. Input isn’t getting through. It may be asleep or off your network.").arg(blocker.hostName)
                      : qsTr("Holding the binding so you can reconnect. Re-plug to keep configuring, or close this screen.")
                color: Theme.muted
                font.pixelSize: Tokens.textSummary
                lineHeight: 1.5
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    ]
}
