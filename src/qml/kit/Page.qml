// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The base every nav destination extends. Transparent, so the window body
// shows through the content area and only the pages' own Cards are surfaces.

// Dish.Chrome is imported UNDER A NAMESPACE on purpose: the module exports its
// own `Page` (this file), so an unqualified import would make the root `Page`
// resolve to this component and qmllint would report an inheritance cycle.
import QtQuick
import QtQuick.Controls.Basic
import Dish.Chrome as Chrome

Page {
    id: page

    default property alias content: body.data

    // false -> the page owns its own layout and pins its own regions (the
    // wizard: banner and footer outside a scrolling body).
    property bool scrollable: true

    background: null

    padding: Chrome.Tokens.pagePadding

    // The one page-level scroller: the rail and breadcrumb stay fixed, only
    // this pane scrolls. contentWidth = availableWidth pins the width so only
    // the vertical axis ever moves.
    contentItem: ScrollView {
        id: scroller
        contentWidth: availableWidth
        clip: true
        ScrollBar.vertical.policy: page.scrollable ? ScrollBar.AsNeeded
                                                   : ScrollBar.AlwaysOff

        Column {
            id: body
            width: scroller.availableWidth
            // Pinned to the viewport when not scrollable, so a child can bind
            // its own height to `parent.height` and own the whole page.
            height: page.scrollable ? implicitHeight : scroller.availableHeight
            spacing: page.scrollable ? Chrome.Tokens.s8 : 0
        }
    }
}
