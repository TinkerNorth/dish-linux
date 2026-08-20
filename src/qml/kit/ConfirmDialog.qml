// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one confirm sheet. `bulletLines` must NAME what is dropped, one item per
// line: "some bindings will be lost" is not a confirm. Reject holds the default
// focus so a stray Enter is never the destructive answer.

// Bound: the bullet delegate resolves against the outer scope.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome

ContentDialog {
    id: confirm

    property string bodyText: ""
    property var bulletLines: []

    focus: true

    onOpened: {
        // The body carries labels only, so the first focusable item in the
        // chain is the reject button.
        const first = confirm.contentItem ? confirm.contentItem.nextItemInFocusChain(true) : null;
        if (first)
            first.forceActiveFocus(Qt.TabFocusReason);
    }

    body: [
        Label {
            visible: confirm.bodyText.length > 0
            text: confirm.bodyText
            color: Theme.muted
            font.pixelSize: Tokens.textSummary
            lineHeight: 1.5
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        },
        Column {
            visible: confirm.bulletLines.length > 0
            spacing: Tokens.s1
            Layout.fillWidth: true

            Repeater {
                model: confirm.bulletLines

                delegate: Row {
                    id: bullet
                    required property string modelData
                    spacing: Tokens.s3

                    Text {
                        text: "·"
                        color: Theme.mutedStrong
                        font.pixelSize: Tokens.textBase
                    }
                    Text {
                        text: bullet.modelData
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                    }
                }
            }
        }
    ]
}
