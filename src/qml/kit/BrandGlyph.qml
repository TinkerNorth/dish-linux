// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Renders `:/brand/*.svg` by bare name. A glyph re-tints by PALETTE, never by
// state: the shipped SVGs bake #8FCFE3, so only a palette whose glyph tint
// differs pays for the colorization pass.
//
// The root id must NOT be named `glyph`: an id shadows a same-named property in
// its own component scope, so `"qrc:/brand/" + glyph` would concatenate the
// Image OBJECT and every glyph in the app would silently render nothing.

import QtQuick
import QtQuick.Effects
import Dish.Chrome

Image {
    id: root

    // Bare brand asset name, e.g. "satellite-connected" (no dir, no ".svg").
    property string glyph: "satellite"
    property bool tinted: true
    // Empty leaves the glyph decorative and ignored by accessibility.
    property string accessibleName: ""

    // The `*-animated.svg` files hold no <animate> and Qt runs no SMIL, so an
    // "-animated" name falls back to its family base; express the transient in
    // QML instead.
    readonly property string asset: root.glyph.indexOf("-animated") >= 0
                                    ? root.glyph.substring(0, root.glyph.indexOf("-"))
                                    : root.glyph

    // Qt.colorEqual, not `!==`: a QColor is never strictly equal to a string.
    readonly property bool recolour: root.tinted && !Qt.colorEqual(Theme.glyph, "#8fcfe3")

    // An empty glyph must not attempt a load — "qrc:/brand/.svg" warns on
    // every create.
    source: root.asset.length > 0 ? "qrc:/brand/" + root.asset + ".svg" : ""
    // Request the exact target px so the SVG rasterises crisp.
    sourceSize.width: width
    sourceSize.height: height
    fillMode: Image.PreserveAspectFit
    smooth: true

    Accessible.role: Accessible.Graphic
    Accessible.name: root.accessibleName
    Accessible.ignored: root.accessibleName.length === 0

    layer.enabled: root.recolour
    layer.effect: MultiEffect {
        colorization: 1.0
        colorizationColor: Theme.glyph
    }

    // Resolve a ConnectionListModel `glyph` role token to a brand asset name.
    function glyphForToken(token) {
        if (token === "satelliteConnected")
            return "satellite-connected";
        if (token === "satelliteOff")
            return "satellite-off";
        return "satellite";            // "satelliteBase" + fallback
    }
}
