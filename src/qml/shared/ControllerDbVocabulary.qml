// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The words for the controller database check's last result, in one place:
// Diagnostics and Settings both say how the last check ended, and two copies
// of the mapping would be two chances to disagree. A total mapping from the
// token the C++ side decided on (core/controllerdb/DbFetch.h) to its words.

import QtQml

QtObject {
    function resultText(token) {
        switch (token) {
        case "upToDate":
            return qsTr("up to date");
        case "updated":
            return qsTr("updated");
        case "offline":
            return qsTr("could not reach dish.tinkernorth.com");
        case "invalid":
            return qsTr("the download did not verify");
        case "off":
            return qsTr("checks are off");
        }
        return "";
    }
}
