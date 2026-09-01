// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The in-flight binding: the UI's one deliberate piece of domain state, living
// exactly as long as the page that instantiates it (no persistence — two
// unbound pads are two independent wizard runs). Both editors instantiate THIS
// file, which is also why whyFor() lives here: the same pad must never get two
// different explanations of why its gyro is dead. Nothing here writes.
//
// REACTIVITY: a JS call is not a binding dependency. Every binding that calls
// capabilityRows() / rowsFor() / annotate() must also read `revision`, bumped
// by every mutator and by the owning page when something the solver reads
// (the type catalog, the radio) moves.

import QtQml

QtObject {
    id: draft

    property string slotId: ""
    property string hostId: ""
    // "satellite" | "bluetooth" | "moonlight"
    property string hostKind: "satellite"
    // -1 = unresolved; never guessed. 0xFF is a real answer for a Moonlight
    // binding: it is the Auto sentinel, resolved against the pad before the
    // wire, and deliberately not 0 (which the wire reads as "host, you pick").
    property int type: -1
    property string desiredPath: "standard" // "standard" | "direct" — never "auto"
    property bool motionOn: true
    property bool rumbleOn: true
    property int touchpadMode: 0          // 0 off · 1 pad · 2 mouse

    // The app this binding will start, or the one it will join. PER SESSION,
    // not per binding: only the binding that creates a session picks one, and
    // every later binding on the same host inherits whatever is running.
    property string appId: ""
    property string appName: ""

    // The solver vends tokens only, but every failure line names something.
    property string padName: ""
    property string hostName: ""
    property string typeName: ""
    // Raw-HID claimable, so "switch to Direct" is advice and not a dead end.
    property bool padClaimable: false
    // The catalog fetch failed outright — a Pending row is a retry, not a wait.
    property bool catalogFailed: false

    // See REACTIVITY above.
    property int revision: 0

    signal changed()

    readonly property bool hasInput: draft.slotId.length > 0
    readonly property bool hasDestination: draft.hostId.length > 0
    // A Bluetooth destination is the system gamepad layer: there is no
    // catalog and so no type to resolve.
    readonly property bool hostIsBluetooth: draft.hostKind === "bluetooth"
    // A Moonlight destination has a type, but no catalog to read it from: the
    // four types are protocol constants, so the draft is answered the moment
    // one is picked and Auto counts as picked.
    readonly property bool hostIsMoonlight: draft.hostKind === "moonlight"
    readonly property bool hasType: draft.hostIsBluetooth || draft.type >= 0
    readonly property bool complete: draft.hasInput && draft.hasDestination && draft.hasType

    // The type step compares three of these side by side without committing.
    function rowsFor(candidateType) {
        if (draft.slotId.length === 0)
            return [];
        return App.capabilityForCandidate(draft.slotId, candidateType, draft.hostKind,
                                          draft.hostId, draft.desiredPath, draft.motionOn,
                                          draft.rumbleOn, draft.touchpadMode);
    }

    function capabilityRows() {
        return draft.rowsFor(draft.type);
    }

    // Adds the display fields Kit.CapabilityTable expects. `includeWhy` is
    // false for the compact table, which has no room for a reason line.
    function annotate(rows, includeWhy) {
        const out = [];
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i];
            row.name = draft.featureName(row.feature);
            row.why = includeWhy ? draft.whyFor(row) : "";
            out.push(row);
        }
        return out;
    }

    function verdictOf(rows, feature) {
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].feature === feature)
                return rows[i].verdict;
        }
        return "pending";
    }

    // Do the four layers carry it at all, ignoring the user gate? Pending is
    // "not read yet", never "no" — an unresolved catalog must not destroy a
    // choice the user already made.
    function layersCarry(rows, feature) {
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i];
            if (row.feature !== feature)
                continue;
            if (row.verdict === "pending")
                return true;
            return row.inOk && row.linkOk && row.typeOk && row.hostOk;
        }
        return true;
    }

    function featureName(feature) {
        switch (feature) {
        case "gamepad":
            return qsTr("Buttons & sticks");
        case "triggers":
            return qsTr("Analog triggers");
        case "motion":
            return qsTr("Gyro");
        case "touchpad":
            return qsTr("Touchpad");
        case "mouse":
            return qsTr("Mouse");
        case "rumble":
            return qsTr("Rumble");
        case "lightbar":
            return qsTr("Lightbar");
        case "triggerEffects":
            return qsTr("Adaptive triggers");
        case "playerLeds":
            return qsTr("Player LEDs");
        }
        return feature;
    }

    // The lowercase noun the failure sentences read as "<x> has no <noun>".
    function featureNoun(feature) {
        switch (feature) {
        case "gamepad":
            return qsTr("buttons or sticks");
        case "triggers":
            return qsTr("analog triggers");
        case "motion":
            return qsTr("gyro");
        case "touchpad":
            return qsTr("touchpad");
        case "mouse":
            return qsTr("mouse control");
        case "rumble":
            return qsTr("rumble motors");
        case "lightbar":
            return qsTr("lightbar");
        case "triggerEffects":
            return qsTr("adaptive triggers");
        case "playerLeds":
            return qsTr("player LEDs");
        }
        return feature;
    }

    // Name the FIRST failing layer and what to do about it. An available row
    // returns "" — there is nothing to explain.
    function whyFor(row) {
        if (row.verdict === "pending") {
            if (draft.hostId.length === 0)
                return qsTr("Waiting on a destination.");
            // A Moonlight binding never waits: its type table is a protocol
            // constant, so nothing here is ever pending on a fetch.

            if (draft.catalogFailed)
                return qsTr("Couldn’t read the catalog from %1 — retry to resolve it.")
                          .arg(draft.hostName);
            return qsTr("Resolving from %1’s catalog…").arg(draft.hostName);
        }
        if (row.verdict === "off") {
            if (row.feature === "touchpad" && draft.touchpadMode === 2)
                return qsTr("Routed to mouse instead.");
            return qsTr("Turned off for this binding.");
        }
        if (row.verdict !== "unavailable" || !row.hasFailingLayer)
            return "";

        switch (row.failingLayer) {
        case "input":
            if (row.feature === "mouse")
                return qsTr("No touchpad on this controller to drive a mouse.");
            return qsTr("%1 has no %2.").arg(draft.padName).arg(draft.featureNoun(row.feature));
        case "link":
            // Which path refuses depends on the feature: Standard has no call
            // for the adaptive triggers or the player LEDs, and Direct is the
            // only path that can reach them. Naming the wrong one sends the
            // user to switch the connection the wrong way.
            if (draft.desiredPath === "direct")
                return qsTr("Direct mode can’t drive it — switch the connection to Standard.");
            return qsTr("Standard mode can’t drive it — switch the connection to Direct.");
        case "type":
            if (draft.hostIsMoonlight)
                return qsTr("A %1 controller does not carry %2 over Moonlight.")
                          .arg(draft.typeName).arg(draft.featureNoun(row.feature));
            return qsTr("%1 doesn’t carry %2.").arg(draft.typeName)
                                               .arg(draft.featureNoun(row.feature));
        case "host":
            if (draft.hostIsBluetooth)
                return qsTr("A Bluetooth host has no channel for it. Bind to a Satellite host.");
            // A Moonlight host never refuses at the host layer: no host reports
            // what it carries, so this branch cannot be reached for one.

            if (row.feature === "mouse")
                return qsTr("%1 doesn’t advertise mouse control.").arg(draft.hostName);
            return qsTr("%1 doesn’t advertise %2.").arg(draft.hostName)
                                                   .arg(draft.featureNoun(row.feature));
        }
        return "";
    }

    // Sanitise the DRAFT, not the render: a touchpad mode the candidate cannot
    // carry collapses to Off here, or a mode the user saw as Off ships and
    // resurrects later. Runs after every host / type / path change.

    function sanitize() {
        const rows = draft.capabilityRows();
        let mode = draft.touchpadMode;
        if (mode === 1 && !draft.layersCarry(rows, "touchpad"))
            mode = 0;
        if (mode === 2 && !draft.layersCarry(rows, "mouse"))
            mode = 0;
        if (mode !== draft.touchpadMode)
            draft.touchpadMode = mode;
        draft.revision += 1;
        draft.changed();
    }

    function chooseInput(id, name, path, claimable) {
        draft.slotId = id;
        draft.padName = name;
        draft.desiredPath = path;
        draft.padClaimable = claimable;
        draft.sanitize();
    }

    function choosePath(path) {
        draft.desiredPath = path;
        draft.sanitize();
    }

    function chooseDestination(id, name, kind) {
        draft.hostId = id;
        draft.hostName = name;
        draft.hostKind = kind;
        // A different host means a different catalog: the old type is a guess
        // until the new catalog resolves it.
        draft.type = -1;
        draft.typeName = "";
        // The app belongs to the destination, so it does not survive one.
        draft.appId = "";
        draft.appName = "";
        draft.sanitize();
    }

    // The app the session will run. Only meaningful while this binding is the
    // one that would CREATE the session; a binding that joins one shows what is
    // already running and is never offered a picker.
    function chooseApp(id, name) {
        draft.appId = id;
        draft.appName = name;
        draft.sanitize();
    }

    function chooseType(candidateType, name) {
        draft.type = candidateType;
        draft.typeName = name;
        draft.sanitize();
    }

    function setMotion(on) {
        draft.motionOn = on;
        draft.sanitize();
    }

    function setRumble(on) {
        draft.rumbleOn = on;
        draft.sanitize();
    }

    function setTouchpad(mode) {
        draft.touchpadMode = mode;
        draft.sanitize();
    }
}
