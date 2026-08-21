// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The entry window. The window manager draws the decorations, so the shell's
// own header is the only chrome Dish paints. It owns the close policies the
// shell cannot see — hide-to-background, the keep-awake confirm and the wizard
// leave guard — and all of them run before the window may go.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Dish.Chrome
import "kit" as Kit

ApplicationWindow {
    id: root
    width: 980
    height: 640
    // The wizard is the tightest surface in the app: two 232px banner slots, a
    // >=60px wire, the rail and the page padding. Below this it clips.
    minimumWidth: Tokens.minWindowWidth
    minimumHeight: Tokens.minWindowHeight
    visible: true
    title: qsTr("Dish")

    // The themed solid. Tiling and compositing WMs both get a window that is
    // opaque everywhere, so no desktop can show through a rounded corner.
    color: Theme.background

    // Set once both guards have cleared, so the re-entrant close() the guard
    // callback issues passes straight through.
    property bool closeApproved: false

    function approveClose() {
        root.closeApproved = true;
        // Qt.quit(), not close(): quitOnLastWindowClosed is false so the process
        // outlives its window, and closing one would only hide it.
        Qt.callLater(function () { Qt.quit(); });
    }

    // The tray item is derived from this, and the shell is the only thing that
    // knows it.
    onVisibleChanged: App.setWindowVisible(root.visible)

    Connections {
        target: App

        function onShowWindowRequested() {
            root.show();
            root.raise();
            root.requestActivate();
        }

        function onQuitRequested() {
            root.approveClose();
        }
    }

    // The desktop's reduced-motion setting can change while Dish runs and no
    // portal signal reaches a Quick app; re-sample whenever we regain focus.
    onActiveChanged: {
        if (root.active)
            Tokens.refreshMotionPreference();
    }

    // Closing is an intent, not a fact: a page holding an unsaved draft gets
    // first refusal, and an active stream is confirmed rather than dropped.
    onClosing: function (close) {
        if (root.closeApproved)
            return;
        close.accepted = false;
        // A hide discards nothing, so it skips the leave guard and the
        // keep-awake confirm: the stream is meant to survive it.
        if (App.requestWindowClose()) {
            root.hide();
            return;
        }
        shell.requestNavigation(function () {
            // Gated on the stream, not on the keep-awake hold: turning keep-awake
            // off must not also remove the confirm before a live stream dies.
            if (App.streamingSlotCount > 0)
                quitConfirm.open();
            else
                root.approveClose();
        });
    }

    // Transparent, so the window's themed body shows through. A first-run flow
    // is pushed here full-screen — over, not inside, the nav shell.
    StackView {
        id: appRoot
        anchors.fill: parent
        background: null

        initialItem: AppShell { id: shell }
    }

    Kit.ConfirmDialog {
        id: quitConfirm
        eyebrow: qsTr("Streaming")
        heading: qsTr("Stop streaming and quit?")
        bodyText: App.keepAwakeReach === "display"
                  ? qsTr("A controller is still streaming, and the display is being kept awake.")
                  : App.keepAwakeReach === "system"
                    ? qsTr("A controller is still streaming, and the computer is being kept awake.")
                    : qsTr("A controller is still streaming.")
        acceptText: qsTr("Quit")
        rejectText: qsTr("Cancel")
        destructiveAccept: true
        onAccepted: {
            quitConfirm.close();
            root.approveClose();
        }
    }

    // Skip is a completion too, or the welcome loops forever.
    Component.onCompleted: {
        if (App.onboardingNeeded) {
            const flow = appRoot.push(Qt.resolvedUrl("onboarding/OnboardingFlow.qml"));
            flow.completed.connect(function (runSetup) {
                appRoot.pop();
                App.markOnboardingComplete();
                if (runSetup)
                    shell.openSetupWizard("");
            });
        }
    }
}
