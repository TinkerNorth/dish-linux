// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Open-source licenses detail sub-page, pushed from Settings ▸ About. Rows come
// from App.licenses() (the bundled manifest); links route through
// App.openExternalUrl so a failure raises the shared toast.

// Bound: the license-row delegate references the outer `licensesPage` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: licensesPage
    title: qsTr("Open source licenses")

    readonly property string headerTitle: qsTr("Open source licenses")
    readonly property string headerSub: qsTr("Acknowledgements for the libraries Dish is built on.")

    // Read once: the manifest is a static resource, so there is nothing to notify on.
    readonly property var entries: App.licenses()

    readonly property int bodyWidth: 640

    // `·` is the app's one separator outside the contract's "<ip> • UDP <port>".
    function metaLine(entry) {
        let parts = [];
        if (entry.version && entry.version.length > 0) { parts.push(entry.version); }
        if (entry.license && entry.license.length > 0) { parts.push(entry.license); }
        if (entry.url && entry.url.length > 0) { parts.push(entry.url); }
        return parts.join(" · ");
    }

    function openUrl(url) {
        if (url && url.length > 0) {
            App.openExternalUrl(url);
        }
    }

    ColumnLayout {
        width: Math.min(licensesPage.bodyWidth, parent ? parent.width : 0)
        spacing: Tokens.s5

        // Every list carries an empty state, even one that is never empty in practice.
        Kit.EmptyState {
            visible: licensesPage.entries.length === 0
            Layout.fillWidth: true
            glyph: "dish-off"
            title: qsTr("No license manifest")
            body: qsTr("The bundled acknowledgements could not be read from this build.")
        }

        Repeater {
            model: licensesPage.entries

            delegate: Kit.RowButton {
                id: licenseRow
                required property var modelData

                Layout.fillWidth: true
                title: licenseRow.modelData.name
                subtitle: licensesPage.metaLine(licenseRow.modelData)
                onClicked: licensesPage.openUrl(licenseRow.modelData.url)
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.topMargin: Tokens.s3
            text: qsTr("Dish %1 · TinkerNorth · LGPL-3.0").arg(App.appVersion)
            color: Theme.muted
            font.family: Tokens.monoFamily
            font.pixelSize: Tokens.textMeta
            wrapMode: Text.WordWrap
        }
    }
}
