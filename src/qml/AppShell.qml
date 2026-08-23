// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The app shell: a collapsible left rail over a content column with a per-page
// header and a StackView. Overlays are IN-SCENE — dialogs are popups over a
// scrim and transient errors land in the one toast host below, never an extra
// OS window. The page-facing contract (`StackView.view.shellApi`, the optional
// header properties, the leave guard) is documented in docs/QML_CONTRACT.md.

// Bound: the inline RailItem component reads the shell's id and its required
// destIndex without falling back to dynamic lookup.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "kit" as Kit

Item {
    id: shell

    // `source` is what the content StackView roots to. The first three render
    // at the rail top, the last two pin to the footer. No two entries may
    // resolve to the same SILHOUETTE — a collapsed 48px icon rail is otherwise
    // unnavigable, and the rule is about the picture, not the file name (see
    // "Rail glyphs" in docs/QML_UI_KIT.md).
    readonly property var destinations: [
        { key: "home",        label: qsTr("Home"),         glyph: "dish-logo",      heart: false, source: "pages/HomePage.qml" },
        { key: "controllers", label: qsTr("Controllers"),  glyph: "pad",            heart: false, source: "pages/ControllersPage.qml" },
        { key: "connections", label: qsTr("Connections"),  glyph: "satellite",      heart: false, source: "pages/ConnectionsPage.qml" },
        { key: "support",     label: qsTr("Support Dish"), glyph: "",               heart: true,  source: "pages/DonatePage.qml" },
        { key: "settings",    label: qsTr("Settings"),     glyph: "gear",           heart: false, source: "pages/SettingsPage.qml" }
    ]

    property int currentIndex: 0
    readonly property bool collapsed: App.railCollapsed

    // The header title fallback when the page declares no headerTitle.
    property string currentTitle: destinations[0].label

    function selectDestination(index) {
        if (index === shell.currentIndex && contentStack.depth <= 1)
            return;
        shell.currentIndex = index;
        shell.currentTitle = shell.destinations[index].label;
        // replace() clears any pushed detail: a rail switch is a fresh root.
        contentStack.replace(null, Qt.resolvedUrl(shell.destinations[index].source));
    }

    // `props` is the initial-property map — ConfigureBindingPage needs its
    // slotId before the component is loaded, not after.
    function pushDetail(url, title, props) {
        shell.currentTitle = title;
        contentStack.push(Qt.resolvedUrl(url), props || {});
    }

    // Always lands on Home's stack: the wizard's result is a Home row, so
    // popping it must never strand the user in Settings. `slotId` pre-answers
    // page 1 for a dangling pad; "" starts at the beginning.
    function openSetupWizard(slotId) {
        shell.selectDestination(0);
        contentStack.push(Qt.resolvedUrl("wizard/SetupWizardPage.qml"),
                          { seedSlotId: slotId || "" });
        shell.currentTitle = qsTr("Set up Dish");
    }

    // Anything that would replace or unwind the stack from OUTSIDE the current
    // page runs through here, so the page gets first refusal. Read through
    // `_cur` (a `var`), never `contentStack.currentItem` (a typed QQuickItem):
    // the guard properties are OPTIONAL page declarations, and a typed read of
    // one makes qmllint's missing-property gate fire.
    function requestNavigation(action) {
        const cur = shell._cur;
        if (cur && cur.blocksLeave === true && typeof cur.requestLeave === "function")
            cur.requestLeave(action);
        else
            action();
    }

    // The one toast host is the shell's, so a page raises a transient notice
    // through here rather than dropping a second host into its own tree.
    function toast(message, severity) {
        toastHost.show(message, severity);
    }

    readonly property var _cur: contentStack.currentItem
    readonly property string headerTitle: _cur && _cur.headerTitle !== undefined
                                          ? _cur.headerTitle : currentTitle
    readonly property string headerSub: _cur && _cur.headerSub !== undefined ? _cur.headerSub : ""
    readonly property string headerDot: _cur && _cur.headerDot !== undefined ? _cur.headerDot : ""
    // The ONE streaming surface: the shell's, not a page's, so it reads the
    // same on every destination and can never disagree with itself. The suffix
    // follows the reach, so a pill can never claim a hold the preferences
    // stopped asking for.
    readonly property string headerPill: App.streamingSlotCount <= 0
                                         ? ""
                                         : App.keepAwakeReach === "display"
                                           ? qsTr("Streaming · display kept awake")
                                           : App.keepAwakeReach === "system"
                                             ? qsTr("Streaming · computer kept awake")
                                             : qsTr("Streaming")
    // A page may suppress the chevron; the wizard does, because it draws its
    // own footer Back and two controls named Back 500px apart is a defect.
    readonly property bool backVisible: contentStack.depth > 1
                                        && !(_cur && _cur.suppressBack === true)

    // The Settings rail item carries a dot while an update waits, so a user who
    // dismissed the popover can still find their way back to it. Only this one
    // phase: checking is silent and a failure is not a call to action.
    readonly property bool updateBadgeVisible: App.updatePhase === "available"

    // Which half of the shell holds the keyboard, for the F6 pane hop.
    readonly property bool railFocused: railToggle.activeFocus || railHome.activeFocus
                                        || railControllers.activeFocus
                                        || railConnections.activeFocus || railAdd.activeFocus
                                        || railSupport.activeFocus || railSettings.activeFocus

    Shortcut {
        sequence: "F6"
        onActivated: {
            if (shell.railFocused) {
                if (contentStack.currentItem)
                    contentStack.currentItem.forceActiveFocus(Qt.TabFocusReason);
            } else {
                railToggle.forceActiveFocus(Qt.TabFocusReason);
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: shell.requestNavigation(function () { shell.selectDestination(4); })
    }
    // Disabled exactly when the chevron is hidden: the wizard then binds
    // Alt+Left to its own footer Back, and two enabled Shortcuts on one
    // sequence is an ambiguous activation.
    Shortcut {
        sequence: "Alt+Left"
        enabled: shell.backVisible
        onActivated: shell.requestNavigation(function () { contentStack.pop(); })
    }

    RowLayout {
        anchors.fill: parent
        // Flush: the rail's own hairline is the seam between the two panes.
        spacing: Tokens.s0

        // ---- Left navigation rail: 236px labeled / 48px icon strip. ----
        Rectangle {
            id: rail
            Layout.fillHeight: true
            Layout.preferredWidth: shell.collapsed ? Tokens.railCompact : Tokens.railExpanded
            color: Theme.surface

            Behavior on Layout.preferredWidth {
                NumberAnimation {
                    duration: Tokens.reducedMotion ? 0 : Tokens.durFast
                    easing.type: Easing.OutCubic
                }
            }

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.outline
            }

            component RailItem: ItemDelegate {
                id: railItem
                required property int destIndex
                readonly property var dest: shell.destinations[destIndex]
                readonly property bool active: destIndex === shell.currentIndex

                implicitHeight: Tokens.navItemHeight
                // The content is positioned in absolute cell coordinates below;
                // control padding would double-inset it.
                padding: Tokens.s0
                focusPolicy: Qt.StrongFocus

                Accessible.role: Accessible.Button
                Accessible.name: railItem.dest.label

                onClicked: shell.requestNavigation(function () {
                    shell.selectDestination(railItem.destIndex);
                })

                // The collapsed rail is icon-only, so the tip is not optional.
                // Declared, never attached — see DishToolTip in QML_UI_KIT.md.
                Kit.DishToolTip {
                    id: railTip
                    visible: (railItem.hovered || railItem.visualFocus) && shell.collapsed
                    delay: 500
                    text: railItem.dest.label
                    x: railItem.width + Tokens.s3
                    y: (railItem.height - railTip.implicitHeight) / 2
                }

                background: Rectangle {
                    radius: Tokens.radiusChip
                    color: railItem.active ? Theme.primaryFill
                         : railItem.hovered ? Theme.primaryHover
                         : "transparent"
                    border.width: railItem.visualFocus ? 1 : 0
                    border.color: Theme.primary

                    Rectangle {   // active-destination accent bar
                        visible: railItem.active
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: Tokens.glyphSm
                        radius: Tokens.radiusBar
                        color: Theme.primary
                    }

                    Rectangle {
                        visible: railItem.visualFocus
                        anchors.fill: parent
                        anchors.margins: -Tokens.s1
                        radius: Tokens.radiusChip + Tokens.s1
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.focusRing
                    }
                }

                contentItem: Item {
                    // Fixed icon cell (railCompact minus the item margins) so the
                    // glyph column doesn't shift as the rail animates.
                    Kit.BrandGlyph {
                        visible: !railItem.dest.heart
                        glyph: railItem.dest.glyph
                        width: Tokens.glyphSm
                        height: Tokens.glyphSm
                        anchors.verticalCenter: parent.verticalCenter
                        x: (Tokens.railCompact - Tokens.s4) / 2 - width / 2
                    }
                    // A text glyph, not a brand SVG, so it rides the palette's
                    // pulse token — the one hue beyond cyan, kept for donations.
                    Label {
                        visible: railItem.dest.heart === true
                        text: "♥"   // brand mark, not localized
                        color: Theme.pulse
                        font.pixelSize: Tokens.textHeading
                        anchors.verticalCenter: parent.verticalCenter
                        x: (Tokens.railCompact - Tokens.s4) / 2 - width / 2
                    }
                    Label {
                        text: railItem.dest.label
                        color: railItem.active ? Theme.onSurface : Theme.muted
                        font.pixelSize: Tokens.textBase
                        font.weight: railItem.active ? Font.DemiBold : Font.Normal
                        anchors.verticalCenter: parent.verticalCenter
                        x: Tokens.railCompact - Tokens.s4
                        opacity: shell.collapsed ? 0 : 1
                        visible: opacity > 0
                        Behavior on opacity {
                            NumberAnimation { duration: Tokens.reducedMotion ? 0 : Tokens.durFast }
                        }
                    }
                    // Pinned to the glyph cell's top-right corner, so it rides
                    // both rail widths without a second layout.
                    Rectangle {
                        visible: railItem.destIndex === 4 && shell.updateBadgeVisible
                        width: Tokens.s3
                        height: Tokens.s3
                        radius: width / 2
                        color: App.updateRequired ? Theme.warning : Theme.primary
                        // The Settings label already names the destination; a
                        // dot with no word of its own would only add noise.
                        Accessible.ignored: true
                        x: (Tokens.railCompact - Tokens.s4) / 2 + Tokens.glyphSm / 2 - width / 2
                        y: (parent.height - Tokens.glyphSm) / 2 - height / 2
                    }
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Tokens.s2
                anchors.bottomMargin: Tokens.s4
                anchors.leftMargin: Tokens.s2
                anchors.rightMargin: Tokens.s2
                // Flush: each RailItem carries its own Layout.margins.
                spacing: Tokens.s0

                // The window manager draws the decorations, so the rail head
                // carries what a title bar would: the app mark and the one
                // control that changes the shell's own shape. Collapsed, the
                // toggle is the only thing left — it is the sole way back.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Tokens.s1
                    Layout.rightMargin: Tokens.s1
                    Layout.bottomMargin: Tokens.s2
                    spacing: Tokens.s2

                    Kit.AppMark {
                        visible: !shell.collapsed
                        Layout.preferredWidth: Tokens.glyphSm
                        Layout.preferredHeight: Tokens.glyphSm
                        Layout.leftMargin: (Tokens.railCompact - Tokens.s4) / 2 - Tokens.glyphSm / 2
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !shell.collapsed
                        text: qsTr("Dish")
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textSummary
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    AbstractButton {
                        id: railToggle
                        Layout.alignment: Qt.AlignHCenter
                        implicitWidth: Tokens.minTouch
                        implicitHeight: Tokens.minTouch
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus

                        Accessible.role: Accessible.Button
                        Accessible.name: shell.collapsed ? qsTr("Expand navigation")
                                                         : qsTr("Collapse navigation")

                        onClicked: App.setRailCollapsed(!shell.collapsed)

                        background: Rectangle {
                            radius: Tokens.radiusChip
                            color: railToggle.hovered ? Theme.primaryHover : "transparent"
                            border.width: railToggle.visualFocus ? 1 : 0
                            border.color: Theme.primary

                            Rectangle {
                                visible: railToggle.visualFocus
                                anchors.fill: parent
                                anchors.margins: -Tokens.s1
                                radius: Tokens.radiusChip + Tokens.s1
                                color: "transparent"
                                border.width: 2
                                border.color: Theme.focusRing
                            }
                        }
                        // Wrapped: a Control resizes its contentItem to the
                        // available box, overriding the glyph's own size.
                        contentItem: Item {
                            Kit.BrandGlyph {
                                glyph: "rail"
                                width: Tokens.glyphSm
                                height: Tokens.glyphSm
                                anchors.verticalCenter: parent.verticalCenter
                                // Collapsed, the toggle heads the icon strip, so
                                // the glyph takes the rail's glyph column less
                                // this button's own centring inset.
                                x: shell.collapsed
                                   ? (Tokens.railCompact - Tokens.s4) / 2 - width / 2 - Tokens.s1
                                   : (parent.width - width) / 2
                            }
                        }

                        // Declared, never attached — see DishToolTip in
                        // QML_UI_KIT.md.
                        Kit.DishToolTip {
                            visible: railToggle.hovered || railToggle.visualFocus
                            delay: 500
                            text: railToggle.Accessible.name
                            y: railToggle.height + Tokens.s2
                        }
                    }
                }

                RailItem { id: railHome;        destIndex: 0; Layout.fillWidth: true; Layout.margins: Tokens.s1 }
                RailItem { id: railControllers; destIndex: 1; Layout.fillWidth: true; Layout.margins: Tokens.s1 }
                RailItem { id: railConnections; destIndex: 2; Layout.fillWidth: true; Layout.margins: Tokens.s1 }

                Item { Layout.fillHeight: true }   // pin the footer cluster

                // An ACTION, not a destination: it opens the wizard on Home's
                // stack. Collapsed, it is the bare centered +.
                AbstractButton {
                    id: railAdd
                    Layout.fillWidth: true
                    Layout.leftMargin: Tokens.s3
                    Layout.rightMargin: Tokens.s3
                    Layout.topMargin: Tokens.s1
                    Layout.bottomMargin: Tokens.s4
                    implicitHeight: 36
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus

                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Set up")

                    onClicked: shell.requestNavigation(function () { shell.openSetupWizard(""); })

                    background: Rectangle {
                        radius: Tokens.radiusButton
                        color: railAdd.down ? Theme.accentWash24
                             : railAdd.hovered ? Theme.primaryPress
                             : Theme.primaryFill
                        border.width: 1
                        border.color: Theme.primary

                        Rectangle {
                            visible: railAdd.visualFocus
                            anchors.fill: parent
                            anchors.margins: -Tokens.s1
                            radius: Tokens.radiusButton + Tokens.s1
                            color: "transparent"
                            border.width: 2
                            border.color: Theme.focusRing
                        }
                    }
                    contentItem: Item {
                        // Fixed + cell (railCompact minus the button margins) so
                        // the glyph column holds as the rail animates.
                        Text {
                            text: "+"
                            color: Theme.primary
                            font.pixelSize: Tokens.textHeading
                            anchors.verticalCenter: parent.verticalCenter
                            x: (Tokens.railCompact - Tokens.s6) / 2 - width / 2
                        }
                        Label {
                            text: qsTr("Set up")
                            color: Theme.primary
                            font.pixelSize: Tokens.textBase
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                            x: Tokens.railCompact - Tokens.s6
                            opacity: shell.collapsed ? 0 : 1
                            visible: opacity > 0
                            Behavior on opacity {
                                NumberAnimation { duration: Tokens.reducedMotion ? 0 : Tokens.durFast }
                            }
                        }
                    }

                    Kit.DishToolTip {
                        id: railAddTip
                        visible: (railAdd.hovered || railAdd.visualFocus) && shell.collapsed
                        delay: 500
                        text: qsTr("Set up")
                        x: railAdd.width + Tokens.s3
                        y: (railAdd.height - railAddTip.implicitHeight) / 2
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: Tokens.s2
                    Layout.rightMargin: Tokens.s2
                    Layout.topMargin: Tokens.s2
                    Layout.bottomMargin: Tokens.s2
                    implicitHeight: 1
                    color: Theme.outline
                }

                RailItem { id: railSupport;  destIndex: 3; Layout.fillWidth: true; Layout.margins: Tokens.s1 }
                RailItem { id: railSettings; destIndex: 4; Layout.fillWidth: true; Layout.margins: Tokens.s1 }

                Label {
                    text: qsTr("Dish %1").arg(App.appVersion)
                    font.family: Tokens.monoFamily
                    font.pixelSize: Tokens.textChip
                    color: Theme.muted
                    Layout.leftMargin: Tokens.s4
                    Layout.topMargin: Tokens.s3
                    Layout.bottomMargin: Tokens.s1
                    opacity: shell.collapsed ? 0 : 1
                    visible: opacity > 0
                    Behavior on opacity {
                        NumberAnimation { duration: Tokens.reducedMotion ? 0 : Tokens.durFast }
                    }
                }
            }
        }

        // ---- Content column: per-page header + StackView. ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Flush: the header owns its own bottom margin.
            spacing: Tokens.s0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Tokens.pagePadding
                Layout.rightMargin: Tokens.pagePadding
                Layout.topMargin: Tokens.s5
                Layout.bottomMargin: Tokens.s2
                spacing: Tokens.s2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Tokens.s6

                    AbstractButton {
                        id: backChevron
                        visible: shell.backVisible
                        implicitWidth: Tokens.minTouch
                        implicitHeight: Tokens.minTouch
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus

                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Back")

                        onClicked: shell.requestNavigation(function () { contentStack.pop(); })

                        background: Rectangle {
                            visible: backChevron.visualFocus
                            radius: Tokens.radiusChip
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.primary

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -Tokens.s1
                                radius: Tokens.radiusChip + Tokens.s1
                                color: "transparent"
                                border.width: 2
                                border.color: Theme.focusRing
                            }
                        }
                        contentItem: Text {
                            text: "‹"
                            // Single guillemets are missing from some mono
                            // faces; pin the family that has them.
                            font.family: Tokens.sansFamily
                            font.pixelSize: Tokens.textTitle
                            color: backChevron.hovered ? Theme.primary : Theme.muted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Label {
                        text: shell.headerTitle
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textTitle
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: shell.headerPill.length > 0
                        implicitWidth: pillText.implicitWidth + Tokens.s8
                        implicitHeight: pillText.implicitHeight + Tokens.s3
                        radius: Tokens.radiusChip
                        color: Theme.primaryFill
                        border.width: 1
                        border.color: Theme.outline

                        Accessible.role: Accessible.StaticText
                        Accessible.name: shell.headerPill

                        Text {
                            id: pillText
                            anchors.centerIn: parent
                            text: shell.headerPill
                            font.family: Tokens.monoFamily
                            font.pixelSize: Tokens.textChip
                            font.letterSpacing: Tokens.sectionLetterSpacing
                            font.capitalization: Font.AllUppercase
                            color: Theme.primary
                        }
                    }
                    Kit.OutlineButton {
                        Layout.alignment: Qt.AlignVCenter
                        visible: shell.headerPill.length > 0
                        size: Kit.DishButton.Small
                        text: qsTr("Configure")
                        Accessible.name: qsTr("Configure keep-awake")
                        onClicked: shell.requestNavigation(function () {
                            shell.selectDestination(4);
                        })
                    }
                    Item { Layout.fillWidth: true }

                    UpdatePill {
                        Layout.preferredWidth: Tokens.minTouch
                        Layout.preferredHeight: Tokens.minTouch
                    }
                }

                RowLayout {
                    visible: shell.headerSub.length > 0
                    spacing: Tokens.s4

                    Kit.StatusDot {
                        token: shell.headerDot
                        visible: shell.headerDot.length > 0
                    }
                    Label {
                        text: shell.headerSub
                        color: Theme.muted
                        font.pixelSize: Tokens.textSummary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }

            StackView {
                id: contentStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                background: null
                // What pages reach the shell through.
                readonly property var shellApi: shell
                // Confines the slide transition to the content bounds, so an
                // outgoing page disappears at the rail divider instead of
                // painting over the rail.
                clip: true
                // Keyed on depth rather than on each pop site, so a page that
                // pops ITSELF (the wizard, on a successful bind) heals the
                // header too.
                onDepthChanged: {
                    if (contentStack.depth <= 1)
                        shell.currentTitle = shell.destinations[shell.currentIndex].label;
                }
                // Not initialItem: that is created during construction, when
                // this is still 0x0 inside the Layout, and the first page then
                // renders blank until some later navigation forces a relayout.
                Component.onCompleted: contentStack.push(Qt.resolvedUrl(shell.destinations[0].source))
            }
        }
    }

    // Dropped ONCE here, so every one-shot failure in the app surfaces as a
    // toast instead of being silently swallowed.
    Kit.NotificationToastHost {
        id: toastHost
        anchors.fill: parent
    }
    Connections {
        target: App
        function onErrorMessage(message) { toastHost.show(message); }
    }

    // The updater's only interruption. Edge-detected in C++ (once per version
    // per session) and success/warning only: a periodic check that failed is a
    // Settings matter, never a toast.
    Connections {
        target: App
        function onUpdateNotice(token, version) {
            if (token === "available")
                toastHost.show(qsTr("Update available: Dish %1. See Settings for the release.")
                               .arg(version), "success");
            else if (token === "unsupported")
                toastHost.show(qsTr("This version of Dish is no longer supported. Please update."),
                               "warning");
        }
    }
}
