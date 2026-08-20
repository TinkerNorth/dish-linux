// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Not dismissable by default. The caller raises `cancellable` only while the
// Connection step is active, where a Direct claim can sit for 20s waiting on
// the kernel to release the device; aborting that falls back to Standard.
// The caller owns the steps and the outcome — this sheet never self-closes.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

ContentDialog {
    id: overlay

    // -> StepList.steps: [{ label, meta, state }]
    property var steps: []
    property bool cancellable: false
    property string slowHint: ""

    signal cancelRequested()

    eyebrow: qsTr("Applying")
    heading: qsTr("Applying binding…")
    preferredWidth: 430

    // A report, not a question: no accept. Reject doubles as the escape.
    acceptText: ""
    rejectText: overlay.cancellable ? qsTr("Cancel") : ""
    closePolicy: overlay.cancellable ? Popup.CloseOnEscape : Popup.NoAutoClose

    onRejected: overlay.cancelRequested()

    body: [
        Label {
            text: qsTr("Sending each setting to the host.")
            color: Theme.muted
            font.pixelSize: Tokens.textSummary
            lineHeight: 1.5
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        },
        StepList {
            steps: overlay.steps
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s1
        },
        Label {
            visible: overlay.slowHint.length > 0
            text: overlay.slowHint
            color: Theme.mutedStrong
            font.pixelSize: Tokens.textMeta
            lineHeight: 1.5
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    ]
}
