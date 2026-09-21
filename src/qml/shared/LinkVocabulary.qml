// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The words for a host link's rank and its protocol standing, in one place.
// C++ vends render TOKENS and never sentences (see src/qml/RenderTokens.h);
// this is the other half of that contract, and it is one file because four
// surfaces name a host, and four copies of the same mapping are four chances
// to disagree about what "fast" says.
//
// Purely declarative and purely presentational: every function is a total
// mapping from a token the C++ side already decided on to the words and the
// tone that render it. JS calls, therefore not binding dependencies: a binding
// that calls one must also read whatever revision its page bumps.

import QtQml
import "../kit" as Kit

QtObject {
    id: vocab

    // ── Link tier ───────────────────────────────────────────────────────────
    // Three words, ranked best-first, the ladder dish-android draws beside its
    // hosts: a Satellite link is the fastest and most stable path, a Moonlight
    // host's control stream is next, and a Bluetooth gamepad link (which the
    // phone offers and this client does not) is the last resort.
    function tierText(token) {
        switch (token) {
        case "fastest":
            return qsTr("Fastest");
        case "fast":
            return qsTr("Fast");
        case "basic":
            return qsTr("Basic");
        }
        return "";
    }

    // Primary for the top rung, plain for the middle, outlined for the last:
    // the rank reads from the fill before the word does.
    function tierTone(token) {
        if (token === "fastest")
            return Kit.CapabilityChip.Present;
        if (token === "fast")
            return Kit.CapabilityChip.Neutral;
        return Kit.CapabilityChip.Absent;
    }

    // ── Protocol standing ───────────────────────────────────────────────────
    // Empty for "current" and "unknown": a satellite that is fine, or one this
    // client has not negotiated with yet, wears no chip at all. Soft amber for
    // a satellite that still works at an older protocol; red when one side
    // must update before a session can open.
    function compatText(token) {
        switch (token) {
        case "satelliteUpdateAvailable":
            return qsTr("Satellite update recommended");
        case "satelliteUpdateRequired":
            return qsTr("Satellite update required");
        case "dishUpdateRequired":
            return qsTr("Dish update required");
        }
        return "";
    }

    function compatTone(token) {
        if (token === "satelliteUpdateAvailable")
            return Kit.CapabilityChip.Warn;
        return Kit.CapabilityChip.Error;
    }
}
