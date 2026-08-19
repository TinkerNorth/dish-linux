// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Host for the first-run welcome pager, pushed full-screen over the nav shell.
// It decides nothing: Main.qml owns whether onboarding is needed, popping, and
// persisting the completed flag. This only reports how the user left, once, via
// `completed(runSetup)`.

import QtQuick

Item {
    id: flow

    signal completed(bool runSetup)

    WelcomeScreen {
        anchors.fill: parent
        onFinished: runSetup => flow.completed(runSetup)
    }
}
