// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Every kit component, every state, one screen, both palettes. There is no
// runtime entry point: it ships in the module so qmllint and qmlcachegen see
// it, and a developer reaches it by pointing Main.qml at it.
//
// The one kit file allowed to touch `App` — the theme switcher is the point.
// QtQuick.Controls.Basic is imported under a namespace because an unqualified
// `Page` would be ambiguous between Controls' Page and this directory's.

import QtQuick
import QtQuick.Controls.Basic as QC
import Dish.Chrome

Item {
    id: gallery

    readonly property var themeOptions: [qsTr("Light"), qsTr("Dark"), qsTr("System")]
    readonly property int bodyWidth: Math.max(320, scroller.availableWidth - 2 * Tokens.pagePadding)

    readonly property var demoSteps: [
        { "label": qsTr("Connection"), "meta": qsTr("standard · kept"), "state": "done" },
        { "label": qsTr("Destination"), "meta": qsTr("sending descriptor"), "state": "active" }
    ]
    readonly property var demoStepsFailed: [
        { "label": qsTr("Connection"), "meta": qsTr("direct · claimed"), "state": "done" },
        { "label": qsTr("Destination"), "meta": qsTr("no answer"), "state": "failed" }
    ]
    readonly property var demoStepsPending: [
        { "label": qsTr("Connection"), "meta": qsTr("standard · kept"), "state": "pending" },
        { "label": qsTr("Destination"), "meta": qsTr("waiting"), "state": "pending" }
    ]

    // One row of each verdict, so Available / Unavailable / Off / Pending are
    // always visible together — Pending must read as a dash, never a cross.
    readonly property var demoCapRows: [
        { "feature": "gamepad", "inOk": true, "linkOk": true, "typeOk": true, "hostOk": true,
          "verdict": "available", "failingLayer": "input", "hasFailingLayer": false,
          "name": qsTr("Buttons & sticks"), "why": "" },
        { "feature": "rumble", "inOk": true, "linkOk": false, "typeOk": true, "hostOk": true,
          "verdict": "unavailable", "failingLayer": "link", "hasFailingLayer": true,
          "name": qsTr("Rumble"),
          "why": qsTr("Direct mode can’t drive it — switch the connection to Standard.") },
        { "feature": "motion", "inOk": true, "linkOk": true, "typeOk": true, "hostOk": true,
          "verdict": "off", "failingLayer": "input", "hasFailingLayer": false,
          "name": qsTr("Gyro"), "why": qsTr("Turned off for this binding.") },
        { "feature": "touchpad", "inOk": false, "linkOk": false, "typeOk": false, "hostOk": false,
          "verdict": "pending", "failingLayer": "input", "hasFailingLayer": false,
          "name": qsTr("Touchpad"), "why": qsTr("Waiting on a destination.") }
    ]

    readonly property var demoChips: [
        { "text": qsTr("as Xbox 360"), "tone": CapabilityChip.Present, "reason": "" },
        { "text": qsTr("standard"), "tone": CapabilityChip.Neutral, "reason": "" },
        { "text": qsTr("rumble on"), "tone": CapabilityChip.Present, "reason": "" },
        { "text": qsTr("no gyro · pad"), "tone": CapabilityChip.Absent,
          "reason": qsTr("This pad has no gyro.") },
        { "text": qsTr("no touchpad · pad"), "tone": CapabilityChip.Absent,
          "reason": qsTr("This pad has no touchpad.") },
        { "text": qsTr("lightbar off"), "tone": CapabilityChip.Absent,
          "reason": qsTr("Turned off for this binding.") },
        { "text": qsTr("dead zones L 12 · R 12"), "tone": CapabilityChip.Neutral, "reason": "" }
    ]

    readonly property var padWaitingSlot: ({ "title": qsTr("Looking for a pad…"),
                                             "sub": qsTr("scanning · usb and bt"),
                                             "empty": true, "hot": true, "tone": "accent" })
    readonly property var padFilledSlot: ({ "title": "ZeroPad Arcade Stick",
                                            "sub": qsTr("USB · Standard · ~250 Hz"),
                                            "empty": false, "hot": false, "tone": "accent" })
    readonly property var hostEmptySlot: ({ "title": qsTr("No destination"),
                                            "sub": qsTr("step 2"),
                                            "empty": true, "hot": false, "tone": "accent" })
    readonly property var hostStaleSlot: ({ "title": qsTr("Searching…"),
                                            "sub": qsTr("0 found"),
                                            "empty": true, "hot": true, "tone": "warn" })
    readonly property var hostFilledSlot: ({ "title": "ORION-PC",
                                             "sub": qsTr("satellite · 3 slots free"),
                                             "empty": false, "hot": false, "tone": "accent" })

    // A cell wider than the Flow's row gets a row of its own.
    component Specimen: Column {
        width: 300
        spacing: Tokens.s4
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    QC.ScrollView {
        id: scroller
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        Column {
            width: scroller.availableWidth
            padding: Tokens.pagePadding
            spacing: Tokens.s9

            SectionHeader {
                label: qsTr("Kit gallery")
                glyph: "dish-master"
            }

            SegmentedControl {
                options: gallery.themeOptions
                value: gallery.themeOptions[Math.max(0, Math.min(2, App.themeMode))]
                onPicked: (option) => App.setThemeMode(gallery.themeOptions.indexOf(option))
            }

            Flow {
                width: gallery.bodyWidth
                spacing: Tokens.s8

                Specimen {
                    Eyebrow { text: qsTr("DishButton") }
                    Row {
                        spacing: Tokens.s4
                        DishButton { text: qsTr("Primary"); variant: DishButton.Primary }
                        DishButton { text: qsTr("Outline"); variant: DishButton.Outline }
                        DishButton { text: qsTr("Unbind"); variant: DishButton.Destructive }
                    }
                    Row {
                        spacing: Tokens.s4
                        DishButton {
                            text: qsTr("Primary")
                            variant: DishButton.Primary
                            enabled: false
                        }
                        DishButton {
                            text: qsTr("Outline")
                            variant: DishButton.Outline
                            enabled: false
                        }
                        DishButton {
                            text: qsTr("Unbind")
                            variant: DishButton.Destructive
                            enabled: false
                        }
                    }
                    Row {
                        spacing: Tokens.s4
                        DishButton {
                            text: qsTr("Small")
                            variant: DishButton.Primary
                            size: DishButton.Small
                        }
                        DishButton {
                            text: qsTr("Edit ›")
                            variant: DishButton.Outline
                            size: DishButton.Small
                        }
                    }
                    Row {
                        spacing: Tokens.s4
                        KitButton { text: qsTr("KitButton alias") }
                        OutlineButton { text: qsTr("OutlineButton alias") }
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("CapabilityChip") }
                    Flow {
                        width: parent.width
                        spacing: Tokens.s2
                        CapabilityChip { text: qsTr("Gyro"); tone: CapabilityChip.Present }
                        CapabilityChip { text: qsTr("No gyro"); tone: CapabilityChip.Absent }
                        CapabilityChip { text: qsTr("12%"); tone: CapabilityChip.Low }
                        CapabilityChip { text: qsTr("Verified"); tone: CapabilityChip.Ok }
                        CapabilityChip { text: qsTr("Layout guessed"); tone: CapabilityChip.Warn }
                        CapabilityChip { text: qsTr("as DualSense"); tone: CapabilityChip.Neutral }
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("StatusDot · LiveStat") }
                    Row {
                        spacing: Tokens.s5
                        StatusDot { token: "success" }
                        StatusDot { token: "warning" }
                        StatusDot { token: "error" }
                        StatusDot { token: "primary" }
                        StatusDot { token: "muted" }
                    }
                    LiveStat {
                        id: measuredRate
                        live: true
                        text: measuredRate.rateText(1000, true)
                    }
                    LiveStat {
                        id: estimatedRate
                        live: false
                        text: estimatedRate.rateText(125, false)
                    }
                    LiveStat {
                        id: latencyStat
                        live: true
                        text: latencyStat.latencyText(3.4, 64)
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("Card") }
                    Card {
                        width: parent.width
                        contentItem: Text {
                            text: qsTr("filled · default inset")
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                        }
                    }
                    Card {
                        width: parent.width
                        filled: false
                        contentItem: Text {
                            text: qsTr("outline only")
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                        }
                    }
                    Card {
                        width: parent.width
                        filled: false
                        dense: true
                        contentItem: Text {
                            text: qsTr("outline · dense")
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                        }
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("ActionCard") }
                    ActionCard {
                        width: parent.width
                        showPlus: true
                        title: qsTr("Set up a controller")
                        subtitle: qsTr("Plug one in, then pick where it goes")
                    }
                    ActionCard {
                        width: parent.width
                        title: qsTr("Bind…")
                        subtitle: qsTr("Pick a destination")
                    }
                    ActionCard {
                        width: parent.width
                        title: qsTr("Bind…")
                        subtitle: qsTr("Pick a destination")
                        enabled: false
                    }
                    ActionCard {
                        width: parent.width
                        placeholder: true
                        title: qsTr("No destination")
                        subtitle: qsTr("step 2")
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("SelectRow") }
                    SelectRow {
                        width: parent.width
                        selected: true
                        glyph: "satellite-connected"
                        dotToken: "success"
                        title: "ORION-PC"
                        subtitle: qsTr("192.168.1.24 · mDNS · 3 slots free")
                        chipText: qsTr("Paired")
                        chipTone: CapabilityChip.Ok
                    }
                    SelectRow {
                        width: parent.width
                        glyph: "satellite"
                        dotToken: "warning"
                        title: "ATTIC-BOX"
                        subtitle: qsTr("192.168.1.31 · LAN · 4 slots free")
                        chipText: qsTr("Needs pairing")
                        chipTone: CapabilityChip.Warn
                    }
                    SelectRow {
                        width: parent.width
                        title: qsTr("ZeroPad Arcade Stick")
                        subtitle: qsTr("USB · Standard")
                        enabled: false
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("OptionCard") }
                    OptionCard {
                        width: parent.width
                        selected: true
                        title: qsTr("Standard")
                        body: qsTr("Read through the system gamepad layer. Works with everything.")
                    }
                    OptionCard {
                        width: parent.width
                        title: qsTr("Direct")
                        body: qsTr("Dish talks to the pad itself. Adds gyro and the touchpad.")
                        badgeText: qsTr("Layout guessed")
                        badgeTone: CapabilityChip.Warn
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("SegmentedControl · ComboButton") }
                    SegmentedControl {
                        options: [qsTr("Standard"), qsTr("Direct")]
                        value: qsTr("Standard")
                    }
                    SegmentedControl {
                        options: [qsTr("Off"), qsTr("Pad"), qsTr("Mouse")]
                        value: qsTr("Pad")
                        small: true
                    }
                    SegmentedControl {
                        options: [qsTr("Standard"), qsTr("Direct")]
                        value: qsTr("Direct")
                        busy: true
                    }
                    SegmentedControl {
                        options: [qsTr("Standard"), qsTr("Direct")]
                        value: qsTr("Standard")
                        enabled: false
                    }
                    ComboButton {
                        options: [qsTr("Xbox 360"), qsTr("DualSense"), qsTr("Switch Pro")]
                        value: qsTr("Xbox 360")
                    }
                }

                // Held open: the state that matters is whether the fill, the
                // hairline and the text read against both palettes.
                Specimen {
                    Eyebrow { text: qsTr("DishToolTip") }
                    Item {
                        width: parent.width
                        height: Tokens.hitRow

                        DishToolTip {
                            visible: true
                            delay: 0
                            text: qsTr("Collapse navigation")
                            x: 0
                            y: 0
                        }
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("LabeledSwitch · RadioMark · SliderRow") }
                    LabeledSwitch {
                        width: parent.width
                        label: qsTr("Motion")
                        description: qsTr("Send the pad’s gyro to the host.")
                        checked: true
                    }
                    LabeledSwitch {
                        width: parent.width
                        label: qsTr("Rumble")
                        description: qsTr("This pad has no motors.")
                        enabled: false
                    }
                    Row {
                        spacing: Tokens.s5
                        RadioMark { selected: true }
                        RadioMark { selected: false }
                        RadioMark { selected: false; enabled: false }
                    }
                    SliderRow {
                        width: parent.width
                        label: qsTr("Left stick")
                        value: 12
                        maxValue: 30
                    }
                    SliderRow {
                        id: gallerySliderMinutes
                        width: parent.width
                        label: qsTr("Let go after")
                        value: 5
                        minValue: 1
                        maxValue: 180
                        valueText: qsTr("%1 min").arg(gallerySliderMinutes.displayValue)
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("KitTextField · RowButton") }
                    KitTextField {
                        width: parent.width
                        placeholderText: qsTr("PIN")
                    }
                    KitTextField {
                        width: parent.width
                        text: "1234"
                        hasError: true
                        errorText: qsTr("That PIN wasn’t accepted. Check the code on ORION-PC and try again.")
                    }
                    RowButton {
                        width: parent.width
                        title: qsTr("Help & FAQ")
                        subtitle: qsTr("What Dish sends, and what it never does.")
                    }
                    RowButton {
                        width: parent.width
                        title: qsTr("Open source licenses")
                        subtitle: qsTr("Everything Dish is built on.")
                        enabled: false
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("DishProgressBar · LoadingSpinner") }
                    DishProgressBar { width: parent.width }
                    DishProgressBar {
                        width: parent.width
                        indeterminate: false
                        value: 0.4
                    }
                    LoadingSpinner {
                        width: parent.width
                        text: qsTr("Resolving from ORION-PC’s catalog…")
                    }
                    LoadingSpinner {
                        width: parent.width
                        running: false
                        text: qsTr("Stopped")
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("ErrorBanner · Callout") }
                    ErrorBanner {
                        width: parent.width
                        text: qsTr("Couldn’t read the catalog from ORION-PC.")
                        detail: qsTr("Retry to resolve it.")
                        showRetry: true
                    }
                    ErrorBanner {
                        width: parent.width
                        tone: ErrorBanner.Warning
                        text: qsTr("Unsteady link")
                        detail: qsTr("Still routing, but packets are dropping. Move closer or switch to USB.")
                    }
                    Callout {
                        width: parent.width
                        tone: Callout.Info
                        text: qsTr("Nothing is changed until the last step.")
                    }
                    Callout {
                        width: parent.width
                        tone: Callout.Warning
                        glyph: "bluetooth-off"
                        text: qsTr("Bluetooth is off on this machine. Turn it on in your system settings to use a wireless pad, or plug one in over USB.")
                        DishButton {
                            text: qsTr("Open Bluetooth settings ↗")
                            variant: DishButton.Outline
                            size: DishButton.Small
                        }
                    }
                    Callout {
                        width: parent.width
                        tone: Callout.Error
                        text: qsTr("ORION-PC stopped responding.")
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("EmptyState · SectionHeader · Eyebrow") }
                    SectionHeader {
                        label: qsTr("Connected")
                        glyph: "dish-connected"
                    }
                    SectionHeader { label: qsTr("Paired") }
                    Eyebrow { text: qsTr("Binding") }
                    Eyebrow {
                        text: qsTr("Binding")
                        mutedTone: true
                    }
                    Card {
                        width: parent.width
                        contentItem: EmptyState {
                            glyph: "dish-off"
                            title: qsTr("No PCs found yet")
                            body: qsTr("Dish looks on your local network. Satellite must be running on the PC you want to drive.")
                            actionText: qsTr("Rescan")
                            showAction: true
                        }
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("BrandGlyph") }
                    Row {
                        spacing: Tokens.s5
                        BrandGlyph {
                            glyph: "dish"
                            width: Tokens.glyphMd
                            height: Tokens.glyphMd
                        }
                        BrandGlyph {
                            glyph: "dish-connected"
                            width: Tokens.glyphMd
                            height: Tokens.glyphMd
                        }
                        BrandGlyph {
                            glyph: "dish-off"
                            width: Tokens.glyphMd
                            height: Tokens.glyphMd
                        }
                        BrandGlyph {
                            glyph: "satellite"
                            width: Tokens.glyphMd
                            height: Tokens.glyphMd
                        }
                        BrandGlyph {
                            glyph: "gear"
                            width: Tokens.glyphMd
                            height: Tokens.glyphMd
                        }
                    }
                    Row {
                        spacing: Tokens.s5
                        BrandGlyph {
                            id: rawGlyph
                            glyph: rawGlyph.glyphForToken("satelliteConnected")
                            tinted: false
                            width: Tokens.glyphLg
                            height: Tokens.glyphLg
                        }
                        BrandGlyph {
                            glyph: "dish-master"
                            accessibleName: qsTr("Dish")
                            width: Tokens.glyphXl
                            height: Tokens.glyphXl
                        }
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("StepList") }
                    StepList {
                        width: parent.width
                        steps: gallery.demoSteps
                    }
                    StepList {
                        width: parent.width
                        steps: gallery.demoStepsFailed
                    }
                    StepList {
                        width: parent.width
                        steps: gallery.demoStepsPending
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("Overlays") }
                    DishButton {
                        text: qsTr("ContentDialog")
                        onClicked: plainDialog.open()
                    }
                    DishButton {
                        text: qsTr("ConfirmDialog")
                        onClicked: discardDialog.open()
                    }
                    DishButton {
                        text: qsTr("ApplyOverlay")
                        onClicked: applyOverlay.open()
                    }
                    DishButton {
                        text: qsTr("BlockerDialog · host")
                        onClicked: {
                            blocker.kind = BlockerDialog.ConnectionLost;
                            blocker.open();
                        }
                    }
                    DishButton {
                        text: qsTr("BlockerDialog · pad")
                        onClicked: {
                            blocker.kind = BlockerDialog.ControllerUnplugged;
                            blocker.open();
                        }
                    }
                    DishButton {
                        text: qsTr("Toast · success")
                        onClicked: toastHost.show(qsTr("Binding applied"), "success")
                    }
                    DishButton {
                        text: qsTr("Toast · warning")
                        onClicked: toastHost.show(qsTr("Direct fell back to Standard"), "warning")
                    }
                    DishButton {
                        text: qsTr("Toast · error")
                        onClicked: toastHost.show(qsTr("Server unreachable — has it moved networks?"),
                                                  "error")
                    }
                }

                Specimen {
                    Eyebrow { text: qsTr("Page") }
                    Page {
                        width: parent.width
                        height: 160
                        title: qsTr("Controllers")

                        SectionHeader { label: qsTr("Connected") }
                        Card {
                            width: parent.width
                            contentItem: Text {
                                text: qsTr("A page inset, on the shell’s ground.")
                                color: Theme.mutedStrong
                                font.pixelSize: Tokens.textSummary
                            }
                        }
                    }
                }

                Specimen {
                    width: gallery.bodyWidth
                    Eyebrow { text: qsTr("WireLine") }
                    WireLine {
                        width: parent.width
                        label: qsTr("idle")
                    }
                    WireLine {
                        width: parent.width
                        live: true
                        label: qsTr("1000 Hz · ~3.4 ms")
                    }
                    WireLine {
                        width: parent.width
                        transmitting: true
                        label: qsTr("binding…")
                    }
                    WireLine {
                        width: parent.width
                        showGlyph: false
                    }
                }

                Specimen {
                    width: gallery.bodyWidth
                    Eyebrow { text: qsTr("CapabilityTable") }
                    CapabilityTable {
                        width: parent.width
                        rows: gallery.demoCapRows
                    }
                    CapabilityTable {
                        width: parent.width
                        compact: true
                        showHeader: false
                        rows: gallery.demoCapRows
                    }
                }

                Specimen {
                    width: gallery.bodyWidth
                    Eyebrow { text: qsTr("BindingStrip") }
                    BindingStrip {
                        width: parent.width
                        availableWidth: parent.width
                        chips: gallery.demoChips
                    }
                    BindingStrip {
                        width: Math.min(parent.width, 420)
                        availableWidth: Math.min(parent.width, 420)
                        chips: gallery.demoChips
                    }
                    BindingStrip {
                        width: Math.min(parent.width, 560)
                        availableWidth: 560
                        chips: gallery.demoChips
                        showEdit: false
                    }
                }

                Specimen {
                    width: gallery.bodyWidth
                    Eyebrow { text: qsTr("WizardBanner") }
                    WizardBanner {
                        width: parent.width
                        stage: 1
                        padSlot: gallery.padWaitingSlot
                        hostSlot: gallery.hostEmptySlot
                    }
                    WizardBanner {
                        width: parent.width
                        stage: 2
                        padSlot: gallery.padFilledSlot
                        hostSlot: gallery.hostStaleSlot
                    }
                    WizardBanner {
                        width: parent.width
                        stage: 3
                        subStep: 2
                        wireLabel: qsTr("as Xbox 360 · rumble")
                        padSlot: gallery.padFilledSlot
                        hostSlot: gallery.hostFilledSlot
                    }
                    WizardBanner {
                        width: parent.width
                        stage: 3
                        subStep: 2
                        transmitting: true
                        wireLabel: qsTr("binding…")
                        padSlot: gallery.padFilledSlot
                        hostSlot: gallery.hostFilledSlot
                    }
                    WizardBanner {
                        width: parent.width
                        compact: true
                        stage: 3
                        subStep: 1
                        wireLabel: qsTr("as Xbox 360")
                        padSlot: gallery.padFilledSlot
                        hostSlot: gallery.hostFilledSlot
                    }
                }
            }
        }
    }

    ContentDialog {
        id: plainDialog
        eyebrow: qsTr("Pairing")
        heading: qsTr("Pair with ORION-PC")
        acceptText: qsTr("Pair")
        body: [
            Text {
                text: qsTr("Enter the six-digit code shown on ORION-PC.")
                color: Theme.muted
                font.pixelSize: Tokens.textSummary
                wrapMode: Text.WordWrap
            }
        ]
    }

    ConfirmDialog {
        id: discardDialog
        eyebrow: qsTr("Setup")
        heading: qsTr("Leave setup?")
        bodyText: qsTr("Nothing has been sent yet. Your answers will be discarded.")
        acceptText: qsTr("Discard")
        rejectText: qsTr("Keep setting up")
        destructiveAccept: true
    }

    ApplyOverlay {
        id: applyOverlay
        steps: gallery.demoSteps
        cancellable: true
        slowHint: qsTr("Handing the device over can take a few seconds.")
    }

    BlockerDialog {
        id: blocker
        hostName: "ORION-PC"
    }

    NotificationToastHost {
        id: toastHost
        anchors.fill: parent
    }
}
