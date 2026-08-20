// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Configure binding — everything about ONE binding, with a live account of what
// will and will not carry. Pushed with `slotId` set BEFORE load. This page and the
// setup wizard are two views of ONE in-flight binding (the same BindingDraft, so a
// pad can never get two explanations). Nothing writes until Apply.

// Bound: the probe/host/type delegates read the outer ids alongside their
// required model properties.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
// BindingDraft resolves through Dish.Chrome; a `import "../shared"` directory
// import would shadow it with a second, incompatible copy of the same file.
import Dish.Chrome
import "../kit" as Kit

Kit.Page {
    id: page

    // Set by the pusher (shellApi.pushDetail(url, title, { slotId })).
    property string slotId: ""

    title: qsTr("Configure binding")

    // ── Shell header contract ───────────────────────────────────────────────
    readonly property string headerTitle: page.padBound ? qsTr("Configure binding")
                                                        : qsTr("Bind controller")
    readonly property string headerDot: page.padLive ? "success" : "muted"
    readonly property string headerSub: page.padName.length > 0
        ? qsTr("%1 · %2").arg(page.linkWord).arg(page.padName)
        : page.linkWord
    readonly property string linkWord: page.padLive ? qsTr("Streaming")
                                     : page.padBound ? qsTr("Bound") : qsTr("Not bound")

    // The page owns its own regions; Kit.Page's scroller is not the body's.
    // The action bar is pinned under a scrolling body (the wizard's layout
    // law), so Unbind/Cancel/Apply never leave the viewport however long the
    // host and type lists grow.
    scrollable: false

    // ── Leave guard ─────────────────────────────────────────────────────────
    // A rail click REPLACES the content stack, so an edited draft has to get
    // first refusal before it is dropped.
    readonly property bool blocksLeave: page.dirty
    property var pendingLeave: null

    function requestLeave(proceed) {
        page.pendingLeave = proceed;
        discardConfirm.open();
    }

    // ── Shell access ────────────────────────────────────────────────────────
    readonly property var shellView: StackView.view
    readonly property var shellApi: page.shellView ? page.shellView.shellApi : null

    function popSelf() {
        if (page.shellView) {
            page.shellView.pop();
        }
    }

    function notify(message, severity) {
        if (page.shellApi) {
            page.shellApi.toast(message, severity);
        }
    }

    // ── Metrics ─────────────────────────────────────────────────────────────
    // Below the min-window content width the matrix goes full width underneath
    // the editor instead of shrinking it toward zero.
    readonly property int labelColumn: 82
    readonly property int matrixWidth: 292
    readonly property int matrixMinWidth: 260
    readonly property bool stacked: page.availableWidth < Tokens.minWindowWidth

    // Two segments, never three: `auto` is a setSlotPath INPUT and never appears
    // in desiredPath, so it is not an answer this page can offer.
    readonly property var pathOptions: [qsTr("Standard"), qsTr("Direct")]

    // ── The pad this page edits ─────────────────────────────────────────────
    // Republished off the slot model so every binding below reads a plain
    // property instead of re-walking a model. Null once the pad is unplugged,
    // which is exactly the signal the terminal blocker needs.
    property var padRow: null

    readonly property string padName: page.padRow ? page.padRow.name : ""
    readonly property bool padBound: page.padRow ? page.padRow.bound : false
    readonly property bool padLive: page.padRow ? page.padRow.live : false
    readonly property bool padBluetooth: page.padRow ? page.padRow.bluetooth : false
    readonly property bool padClaimable: page.padRow
                                         ? page.padRow.pathSupported && !page.padRow.bluetooth
                                         : false
    readonly property bool padVerified: page.padRow ? page.padRow.verifiedModel : false
    readonly property string padPhase: page.padRow ? page.padRow.pathPhase : ""
    readonly property string padFailure: page.padRow ? page.padRow.directFailure : ""
    readonly property bool padClaiming: page.padRow ? page.padRow.claimInProgress : false

    // The USB path FSM's reasons. The Direct control here is the same control the
    // slot card carries, so it gets the same INLINE reason — never a toast, and
    // never a silent revert.
    readonly property string pathNote: {
        if (page.padFailure === "permissionDenied") {
            return qsTr("Direct access denied — hidraw needs a udev rule. Install packaging/udev/70-dish-hidraw.rules, run udevadm control --reload-rules && udevadm trigger, then replug the controller.");
        }
        if (page.padPhase === "needsReplug") {
            return qsTr("Unplug and replug the controller to finish switching.");
        }
        if (page.padPhase === "restoreStuck") {
            return qsTr("The controller didn’t come back cleanly from Direct — replug it.");
        }
        if (page.padClaiming) {
            return qsTr("Claiming controller…");
        }
        return "";
    }

    function adoptPad(row, isTarget) {
        if (isTarget) {
            page.padRow = row;
            page.seed();
        } else if (page.padRow === row) {
            page.padRow = null;
        }
    }

    // ── The chosen destination's live row ───────────────────────────────────
    property var hostRow: null

    readonly property bool hostLive: page.hostRow ? page.hostRow.liveLink : false
    readonly property string hostLink: page.hostRow ? page.hostRow.linkState : ""

    function adoptHost(row, isChosen) {
        if (isChosen) {
            page.hostRow = row;
            draft.hostName = row.label;
        } else if (page.hostRow === row) {
            page.hostRow = null;
        }
    }

    // ── The draft (one file, two editors) ───────────────────────────────────
    BindingDraft {
        id: draft
        slotId: page.slotId
        // A Pending row is a retry rather than a wait once the fetch has failed
        // outright with nothing cached.
        catalogFailed: page.catalogBroken
    }

    // The snapshot the page opened with; `dirty` is read off it, so a rail click
    // never confirms on a page the user only looked at.
    // NOT `baseline`: Item declares that anchor line FINAL, and shadowing it
    // makes the whole component fail to create at RUNTIME (the page never opens).
    property var openedWith: null
    property bool seeded: false

    readonly property bool dirty: page.openedWith !== null
        && (draft.hostId !== page.openedWith.hostId
            || draft.type !== page.openedWith.type
            || draft.desiredPath !== page.openedWith.desiredPath
            || draft.motionOn !== page.openedWith.motionOn
            || draft.rumbleOn !== page.openedWith.rumbleOn
            || draft.touchpadMode !== page.openedWith.touchpadMode)

    function snapshot() {
        page.openedWith = {
            "hostId": draft.hostId,
            "type": draft.type,
            "desiredPath": draft.desiredPath,
            "motionOn": draft.motionOn,
            "rumbleOn": draft.rumbleOn,
            "touchpadMode": draft.touchpadMode
        };
    }

    // Seed the draft from what is bound TODAY. Runs once, as soon as the pad row
    // exists — the model can populate after this page is constructed.
    function seed() {
        if (page.seeded || !page.padRow) {
            return;
        }
        page.seeded = true;

        draft.chooseInput(page.slotId, page.padRow.name,
                          page.padRow.desiredPath === "direct" ? "direct" : "standard",
                          page.padClaimable);

        const boundId = page.padRow.boundConnectionId;
        if (boundId.length > 0) {
            // The real label arrives later with the host row. Every destination
            // this page can offer is a Satellite — there is no BT-host source.
            draft.chooseDestination(boundId, boundId, "satellite");
            // Keyed on the DESTINATION, never on the pad: in bind mode the pad has
            // no binding, and the slot-keyed read resolves through one.
            page.refreshCatalog();
            const current = App.emulateCurrentTypeForHost(boundId, page.slotId);
            if (current >= 0) {
                draft.chooseType(current, page.typeNameFor(current));
            }
            draft.touchpadMode = page.touchpadIndex(App.touchpadModeFor(boundId));
        }
        draft.motionOn = App.motionEnabledFor(page.slotId);
        draft.rumbleOn = App.rumbleEnabledFor(page.slotId);

        draft.sanitize();
        page.snapshot();
    }

    Component.onCompleted: page.seed()

    // ── The controller-type catalog ─────────────────────────────────────────
    // emulateTypes is a one-shot read, so it is re-pulled on every catalog move;
    // the same edge bumps the draft, because the solver reads the catalog and a
    // function call is not a binding dependency.
    property var types: []

    readonly property bool catalogLoading: App.emulateLoading
    readonly property bool catalogBroken: App.emulateError.length > 0 && page.types.length === 0

    function reloadTypes() {
        page.types = App.emulateTypesForHost(draft.hostId);
    }

    // A different destination is a different catalog.
    function refreshCatalog() {
        if (draft.hasDestination && !draft.hostIsBluetooth) {
            App.refreshEmulateForHost(draft.hostId);
        }
        page.reloadTypes();
    }

    Connections {
        target: draft
        function onHostIdChanged() {
            if (page.seeded) {
                page.refreshCatalog();
            }
        }
    }

    function typeNameFor(wireType) {
        for (let i = 0; i < page.types.length; ++i) {
            if (page.types[i].type === wireType) {
                return page.types[i].name;
            }
        }
        return "";
    }

    Connections {
        target: App

        function onEmulateStateChanged() {
            page.reloadTypes();
            // The type name may only now be resolvable for a pre-seeded type.
            if (draft.type >= 0 && draft.typeName.length === 0) {
                draft.typeName = page.typeNameFor(draft.type);
            }
            draft.sanitize();
        }

        function onApplyFinished(ok, reasonToken, directFellBack) {
            page.handleApplyResult(ok, reasonToken, directFellBack);
        }
    }

    // ── The matrix ──────────────────────────────────────────────────────────
    // capabilityRows()/annotate() are FUNCTION CALLS and therefore not binding
    // dependencies: `draft.revision` is what makes this re-evaluate (see the
    // REACTIVITY note in shared/BindingDraft.qml).
    readonly property var matrixRows: draft.revision >= 0
                                      ? draft.annotate(draft.capabilityRows(), true) : []

    readonly property int carriedCount: {
        let n = 0;
        for (let i = 0; i < page.matrixRows.length; ++i) {
            if (page.matrixRows[i].verdict === "available") {
                ++n;
            }
        }
        return n;
    }

    // Pending is uniform across the table: no host, or a catalog that has not
    // resolved. Never a cross drawn from an unresolved catalog.
    readonly property bool matrixPending: {
        if (page.matrixRows.length === 0) {
            return true;
        }
        for (let i = 0; i < page.matrixRows.length; ++i) {
            if (page.matrixRows[i].verdict !== "pending") {
                return false;
            }
        }
        return true;
    }

    // Does every layer carry it, ignoring the user's own switch? That is the
    // question a tune row asks: a feature the user turned OFF still needs its
    // control, or there is no way back on.
    function carries(feature) {
        for (let i = 0; i < page.matrixRows.length; ++i) {
            const row = page.matrixRows[i];
            if (row.feature === feature) {
                return row.verdict === "available" || row.verdict === "off";
            }
        }
        return false;
    }

    function availableNames(features) {
        const out = [];
        for (let i = 0; i < page.matrixRows.length; ++i) {
            const row = page.matrixRows[i];
            if (features.indexOf(row.feature) >= 0 && row.verdict === "available") {
                out.push(row.name);
            }
        }
        return out;
    }

    readonly property var getsNames: page.availableNames(["gamepad", "motion", "touchpad", "mouse"])
    readonly property var sendsNames: page.availableNames(["rumble"])

    // The instrument-panel strip under the matrix: what the draft actually holds,
    // in the wire's own words.
    readonly property string panelFooter: [
        qsTr("slot %1").arg(page.slotId),
        draft.desiredPath,
        draft.hostId.length > 0 ? draft.hostId : qsTr("unbound"),
        draft.type >= 0 ? qsTr("type %1").arg(draft.type) : qsTr("type —")
    ].join(" · ")

    // ── Tune rows ───────────────────────────────────────────────────────────
    readonly property bool motionTunable: page.carries("motion")
    readonly property bool rumbleTunable: page.carries("rumble")
    readonly property bool touchpadTunable: page.carries("touchpad") || page.carries("mouse")
    readonly property bool nothingTunable: draft.hasDestination && draft.hasType
                                           && !page.motionTunable && !page.rumbleTunable
                                           && !page.touchpadTunable

    readonly property var touchpadOptions: [qsTr("Off"), qsTr("Pad"), qsTr("Mouse")]

    function touchpadIndex(token) {
        if (token === "pad") {
            return 1;
        }
        if (token === "mouse") {
            return 2;
        }
        return 0;
    }

    // ── Link vocabulary ─────────────────────────────────────────────────────
    function chipText(token) {
        switch (token) {
        case "found":
            return qsTr("Found");
        case "needsPairing":
            return qsTr("Needs pairing");
        case "offline":
            return qsTr("Offline");
        case "ready":
            return qsTr("Ready");
        case "connecting":
            return qsTr("Connecting…");
        case "online":
            return qsTr("Online");
        case "unstable":
            return qsTr("Unsteady");
        default:
            return token;
        }
    }

    function chipTone(token) {
        switch (token) {
        case "online":
            return Kit.CapabilityChip.Ok;
        case "connecting":
        case "unstable":
        case "needsPairing":
            return Kit.CapabilityChip.Warn;
        case "ready":
            return Kit.CapabilityChip.Present;
        default:
            return Kit.CapabilityChip.Neutral;
        }
    }

    // Never assert a slot NUMBER before bindSlot allocates one — pre-bind, the
    // host says how much room it has, not where the pad lands. `boundCount` is
    // only a reactivity token: a call is not a binding dependency.
    function hostSubtitle(connectionId, boundCount) {
        const free = App.hostSlotCapacity() - App.hostBoundSlotCount(connectionId);
        if (free > 0) {
            return qsTr("%n slots free", "", free);
        }
        const displaced = App.displacedSlotName(connectionId);
        return displaced.length > 0
            ? qsTr("0 slots free — %1 will be unbound").arg(displaced)
            : qsTr("0 slots free — one pad will be unbound");
    }

    // ── Blockers ────────────────────────────────────────────────────────────
    readonly property bool padGone: page.seeded && !page.padRow
    property bool hostWasLive: false
    onHostLiveChanged: if (page.hostLive) {
        page.hostWasLive = true;
    }
    readonly property bool hostLost: page.hostWasLive && !page.hostLive
    readonly property bool blocked: page.padGone || page.hostLost

    onBlockedChanged: {
        if (page.blocked) {
            blocker.open();
        } else {
            blocker.close();
        }
    }

    // The unsteady link is inline, and it waits: a link that flickers for under
    // three seconds has said nothing worth interrupting for. The dismiss is
    // re-armed only by a return to a steady link.
    readonly property bool hostUnstable: page.hostLink === "unstable"
    readonly property bool hostSteady: page.hostLink === "connected"
    property bool unsteadyRaised: false
    property bool unsteadyDismissed: false
    readonly property bool unsteadyShown: page.unsteadyRaised && !page.unsteadyDismissed

    Timer {
        id: unsteadyHold
        interval: 3000
        repeat: false
        onTriggered: page.unsteadyRaised = true
    }

    onHostUnstableChanged: {
        if (page.hostUnstable) {
            unsteadyHold.restart();
        } else {
            unsteadyHold.stop();
            page.unsteadyRaised = false;
        }
    }
    onHostSteadyChanged: if (page.hostSteady) {
        page.unsteadyDismissed = false;
    }

    // ── Apply ───────────────────────────────────────────────────────────────
    readonly property bool canApply: page.padRow !== null && draft.hasDestination && draft.hasType
    readonly property bool noHosts: App.connectionModel.count === 0
    property bool applyRequested: false

    readonly property string actionHint: !draft.hasDestination
        ? qsTr("Pick a destination to continue.")
        : !draft.hasType ? qsTr("Waiting on the controller catalog.")
                         : qsTr("Nothing is sent until you apply.")

    function stepState(token) {
        // A skipped step is a step that will not run: drawn done, captioned so
        // the caption says why. It is never a pending row that never moves.
        return token === "skipped" ? "done" : token;
    }

    readonly property var applySteps: [
        {
            "label": qsTr("Connection"),
            "meta": App.applyConnectionState === "skipped"
                    ? qsTr("standard · kept")
                    : (draft.desiredPath === "direct" ? qsTr("claiming") : qsTr("standard")),
            "state": page.stepState(App.applyConnectionState)
        },
        {
            "label": qsTr("Destination"),
            "meta": qsTr("sending descriptor"),
            "state": page.stepState(App.applyDestinationState)
        }
    ]

    function apply() {
        page.applyRequested = true;
        applyOverlay.open();
        App.applyBinding(page.slotId, draft.hostId, draft.type, draft.desiredPath,
                         draft.motionOn, draft.rumbleOn, draft.touchpadMode);
    }

    function failureText(reasonToken) {
        switch (reasonToken) {
        case "slotGone":
            return qsTr("%1 is no longer available. Unplug and reconnect it, then try again.")
                     .arg(page.padName);
        case "hostUnreachable":
            return qsTr("Couldn’t reach %1. Make sure it’s on and on your network.")
                     .arg(draft.hostName);
        case "bindRejected":
            return qsTr("%1 refused the binding.").arg(draft.hostName);
        case "cancelled":
            return qsTr("Apply cancelled. Nothing was changed.");
        }
        return qsTr("Couldn’t apply the binding.");
    }

    // Success pops back to Home's row; a failure keeps this page and its draft
    // exactly as they were, with the reason in the toast and Apply live again.
    function handleApplyResult(ok, reasonToken, directFellBack) {
        if (!page.applyRequested) {
            return;
        }
        page.applyRequested = false;
        applyOverlay.close();
        if (directFellBack) {
            page.notify(qsTr("Direct wasn’t available — bound on Standard instead."), "warning");
        }
        if (ok) {
            page.notify(qsTr("Controller bound — %1 is live on %2.")
                          .arg(page.padName).arg(draft.hostName), "success");
            page.snapshot();
            page.popSelf();
        } else {
            page.notify(page.failureText(reasonToken), "error");
        }
    }

    function unbind() {
        App.unbindSlot(page.slotId);
        page.notify(qsTr("Binding removed — %1 is no longer driving a host.").arg(page.padName),
                    "success");
    }

    function leave() {
        if (page.dirty) {
            page.requestLeave(page.popSelf);
        } else {
            page.popSelf();
        }
    }

    // ── Model probes ────────────────────────────────────────────────────────
    // Repeaters, not ListViews, so every row is instantiated: a virtualised
    // delegate that was never created could not adopt itself.
    Item {
        visible: false

        Repeater {
            model: App.slotModel

            delegate: Item {
                id: padProbe

                required property string slotId
                required property string name
                required property bool bound
                required property string boundConnectionId
                required property bool live
                required property bool bluetooth
                required property bool pathSupported
                required property string pathPhase
                required property string desiredPath
                required property bool claimInProgress
                required property string directFailure
                required property bool hasMotion
                required property bool hasTouchpad
                required property bool hasLightbar
                required property bool hasRumble
                required property bool verifiedModel
                required property int gamepadHz
                required property bool gamepadHzLive

                readonly property bool isTarget: padProbe.slotId === page.slotId

                onIsTargetChanged: page.adoptPad(padProbe, padProbe.isTarget)
                Component.onCompleted: page.adoptPad(padProbe, padProbe.isTarget)
                Component.onDestruction: page.adoptPad(padProbe, false)
            }
        }
    }

    // ── Body ────────────────────────────────────────────────────────────────
    ColumnLayout {
        width: parent.width
        height: parent.height
        spacing: Tokens.s6

        // ── Scrolling body ──────────────────────────────────────────────────
        ScrollView {
            id: bodyScroll
            clip: true
            contentWidth: availableWidth
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                width: bodyScroll.availableWidth
                spacing: Tokens.s6

                // In flow above the columns, not over them: the page stays fully
                // usable on an unsteady link, because unstable IS a live link.
                Kit.ErrorBanner {
                    visible: page.unsteadyShown
                    Layout.fillWidth: true
                    tone: Kit.ErrorBanner.Warning
                    text: qsTr("Unsteady link")
                    detail: qsTr("Still routing, but packets are dropping. Move closer or switch to USB.")
                    showRetry: true
                    retryText: qsTr("Dismiss")
                    onRetryRequested: page.unsteadyDismissed = true
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: page.stacked ? 1 : 2
                    columnSpacing: Tokens.s8
                    rowSpacing: Tokens.s8

                    // ══ EDITOR COLUMN ═══════════════════════════════════════════════
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Tokens.s6

                        // ── INPUT ───────────────────────────────────────────────────
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.Eyebrow { mutedTone: true; text: qsTr("Input") }
                            Label {
                                Layout.fillWidth: true
                                text: page.padName
                                color: Theme.mutedStrong
                                font.family: Tokens.monoFamily
                                font.pixelSize: Tokens.textMeta
                                elide: Text.ElideRight
                            }
                        }

                        Kit.Card {
                            Layout.fillWidth: true

                            contentItem: ColumnLayout {
                                spacing: Tokens.s5

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Tokens.s4

                                    Kit.Eyebrow {
                                        mutedTone: true
                                        text: qsTr("Connection")
                                        Layout.preferredWidth: page.labelColumn
                                    }
                                    Kit.CapabilityChip {
                                        tone: Kit.CapabilityChip.Neutral
                                        text: page.padBluetooth ? qsTr("Bluetooth") : qsTr("USB")
                                    }
                                    Kit.LiveStat {
                                        id: padRate
                                        live: page.padLive
                                        text: page.padRow
                                              ? padRate.rateText(page.padRow.gamepadHz,
                                                                 page.padRow.gamepadHzLive)
                                              : ""
                                        visible: padRate.text.length > 0
                                    }
                                    Item { Layout.fillWidth: true }

                                    Kit.SegmentedControl {
                                        visible: page.padClaimable
                                        small: true
                                        busy: page.padClaiming
                                        options: page.pathOptions
                                        value: page.pathOptions[draft.desiredPath === "direct" ? 1 : 0]
                                        onPicked: option => draft.choosePath(
                                                      page.pathOptions.indexOf(option) === 1
                                                      ? "direct" : "standard")
                                    }
                                    // The trust judgement rides the PATH, not the pad:
                                    // the risk it names exists only on Direct.
                                    Kit.CapabilityChip {
                                        visible: page.padClaimable && !page.padVerified
                                        tone: Kit.CapabilityChip.Warn
                                        text: qsTr("Layout guessed")
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: page.pathNote.length > 0
                                    text: page.pathNote
                                    color: page.padFailure === "permissionDenied" ? Theme.error
                                                                                  : Theme.warning
                                    font.pixelSize: Tokens.textMeta
                                    wrapMode: Text.WordWrap
                                }

                                Kit.Callout {
                                    visible: page.padClaimable && !page.padVerified
                                             && draft.desiredPath === "direct"
                                    Layout.fillWidth: true
                                    tone: Kit.Callout.Warning
                                    text: qsTr("Not recognised. Direct mode guesses this controller’s layout, so some inputs may read wrong. You can switch back anytime.")
                                }

                                Kit.Callout {
                                    visible: page.padRow !== null && !page.padClaimable
                                    Layout.fillWidth: true
                                    tone: Kit.Callout.Info
                                    text: qsTr("Direct mode needs a USB connection. Over Bluetooth this pad runs on the Standard path.")
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: Theme.outlineSubtle
                                }

                                // An absent capability is still drawn, negated and legible.
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Tokens.s4

                                    Kit.Eyebrow {
                                        mutedTone: true
                                        text: qsTr("Functions")
                                        Layout.preferredWidth: page.labelColumn
                                        Layout.alignment: Qt.AlignTop
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: Tokens.s2

                                        Kit.CapabilityChip {
                                            id: rumbleChip
                                            readonly property bool has: page.padRow
                                                                        ? page.padRow.hasRumble : false
                                            text: rumbleChip.has ? qsTr("Rumble") : qsTr("No rumble")
                                            tone: rumbleChip.has ? Kit.CapabilityChip.Present
                                                                 : Kit.CapabilityChip.Absent
                                        }
                                        Kit.CapabilityChip {
                                            id: gyroChip
                                            readonly property bool has: page.padRow
                                                                        ? page.padRow.hasMotion : false
                                            text: gyroChip.has ? qsTr("Gyro") : qsTr("No gyro")
                                            tone: gyroChip.has ? Kit.CapabilityChip.Present
                                                               : Kit.CapabilityChip.Absent
                                        }
                                        Kit.CapabilityChip {
                                            id: touchpadChip
                                            readonly property bool has: page.padRow
                                                                        ? page.padRow.hasTouchpad : false
                                            text: touchpadChip.has ? qsTr("Touchpad")
                                                                   : qsTr("No touchpad")
                                            tone: touchpadChip.has ? Kit.CapabilityChip.Present
                                                                   : Kit.CapabilityChip.Absent
                                        }
                                        Kit.CapabilityChip {
                                            visible: page.padRow ? page.padRow.hasLightbar : false
                                            text: qsTr("Lightbar")
                                            tone: Kit.CapabilityChip.Present
                                        }
                                    }
                                }
                            }
                        }

                        // ── DESTINATION ─────────────────────────────────────────────
                        Kit.Eyebrow {
                            mutedTone: true
                            text: qsTr("Destination")
                            Layout.topMargin: Tokens.s2
                        }

                        Kit.EmptyState {
                            visible: page.noHosts
                            Layout.fillWidth: true
                            glyph: "satellite-off"
                            title: qsTr("No hosts found")
                            body: page.padName.length > 0
                                  ? qsTr("Scan your network for a Satellite host, or pair one over Bluetooth. Once a host appears, come back to bind %1.").arg(page.padName)
                                  : qsTr("Scan your network for a Satellite host, or pair one over Bluetooth.")
                            actionText: qsTr("Manage destinations ›")
                            showAction: true
                            onActionRequested: if (page.shellApi) {
                                page.shellApi.selectDestination(2);
                            }
                        }

                        // Every host except one already carrying another pad; this slot's
                        // own binding is always offered, so an offline host it is
                        // already bound to can still be re-picked.
                        Repeater {
                            model: App.connectionModel

                            delegate: Kit.SelectRow {
                                id: hostOption

                                // `glyph` is deliberately NOT pulled: SelectRow already
                                // declares a `glyph` property and a delegate cannot
                                // redeclare one that exists on its base type.
                                required property string connectionId
                                required property string label
                                required property string ip
                                required property string linkState
                                required property string chip
                                required property string dotColor
                                required property string boundSlotId
                                required property bool liveLink
                                required property string latencyText
                                required property int latencySamples

                                readonly property bool offerable: hostOption.boundSlotId.length === 0
                                                                  || hostOption.boundSlotId === page.slotId
                                readonly property bool chosen: draft.hostId === hostOption.connectionId
                                // Latency is drawn only on a genuinely live link with
                                // samples — never "~0 ms".
                                readonly property bool showLatency: hostOption.latencySamples > 0
                                    && (hostOption.linkState === "connected"
                                        || hostOption.chip === "unstable")

                                visible: hostOption.offerable
                                Layout.fillWidth: true

                                selected: hostOption.chosen
                                title: hostOption.label
                                subtitle: hostOption.ip + " · "
                                          + page.hostSubtitle(hostOption.connectionId, App.boundSlotCount)
                                          + (hostOption.showLatency ? " · " + hostOption.latencyText : "")
                                dotToken: hostOption.dotColor
                                chipText: page.chipText(hostOption.chip)
                                chipTone: page.chipTone(hostOption.chip)

                                onPicked: draft.chooseDestination(hostOption.connectionId,
                                                                  hostOption.label, "satellite")

                                onChosenChanged: page.adoptHost(hostOption, hostOption.chosen)
                                Component.onCompleted: page.adoptHost(hostOption, hostOption.chosen)
                                Component.onDestruction: page.adoptHost(hostOption, false)
                            }
                        }

                        Kit.Callout {
                            visible: draft.hostIsBluetooth
                            Layout.fillWidth: true
                            tone: Kit.Callout.Info
                            text: qsTr("This machine pairs as a Bluetooth gamepad. Gyro, touchpad and mouse need a Satellite host.")
                        }

                        RowLayout {
                            visible: draft.hasDestination
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.Eyebrow {
                                mutedTone: true
                                text: qsTr("Gets")
                                Layout.preferredWidth: page.labelColumn
                                Layout.alignment: Qt.AlignTop
                            }
                            Label {
                                visible: page.matrixPending
                                Layout.fillWidth: true
                                text: qsTr("Waiting on the type…")
                                color: Theme.mutedStrong
                                font.pixelSize: Tokens.textMeta
                                font.italic: true
                            }
                            Flow {
                                visible: !page.matrixPending
                                Layout.fillWidth: true
                                spacing: Tokens.s2

                                Repeater {
                                    model: page.getsNames

                                    delegate: Kit.CapabilityChip {
                                        id: getsChip
                                        required property string modelData
                                        text: getsChip.modelData
                                        tone: Kit.CapabilityChip.Present
                                    }
                                }
                            }
                        }

                        RowLayout {
                            visible: draft.hasDestination
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.Eyebrow {
                                mutedTone: true
                                text: qsTr("Sends back")
                                Layout.preferredWidth: page.labelColumn
                                Layout.alignment: Qt.AlignTop
                            }
                            Label {
                                visible: page.matrixPending || page.sendsNames.length === 0
                                Layout.fillWidth: true
                                text: page.matrixPending ? qsTr("Waiting on the type…")
                                                         : qsTr("No return channel")
                                color: Theme.mutedStrong
                                font.pixelSize: Tokens.textMeta
                                font.italic: true
                            }
                            Flow {
                                visible: !page.matrixPending && page.sendsNames.length > 0
                                Layout.fillWidth: true
                                spacing: Tokens.s2

                                Repeater {
                                    model: page.sendsNames

                                    delegate: Kit.CapabilityChip {
                                        id: sendsChip
                                        required property string modelData
                                        text: sendsChip.modelData
                                        tone: Kit.CapabilityChip.Present
                                    }
                                }
                            }
                        }

                        // ── BINDING ─────────────────────────────────────────────────
                        Kit.SectionHeader {
                            visible: draft.hasDestination
                            label: qsTr("Binding")
                            Layout.topMargin: Tokens.s2
                        }

                        Kit.Eyebrow {
                            visible: draft.hasDestination
                            mutedTone: true
                            text: qsTr("Emulate as")
                        }

                        // Bluetooth destination: not a question. The HID profile fixes
                        // the type, so the page states it rather than asking.
                        RowLayout {
                            visible: draft.hasDestination && draft.hostIsBluetooth
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.CapabilityChip {
                                tone: Kit.CapabilityChip.Neutral
                                text: qsTr("Gamepad")
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Fixed by the Bluetooth HID profile.")
                                color: Theme.mutedStrong
                                font.pixelSize: Tokens.textMeta
                                wrapMode: Text.WordWrap
                            }
                        }

                        // Loading: never a guessed default, and never a bare spinner.
                        Kit.LoadingSpinner {
                            visible: draft.hasDestination && !draft.hostIsBluetooth && page.catalogLoading
                            Layout.fillWidth: true
                            running: page.catalogLoading
                            text: qsTr("Reading the controller catalog from %1…").arg(draft.hostName)
                        }

                        // Failed with nothing cached: tap to retry, Apply stays disabled
                        // because draft.hasType is false.
                        Kit.ErrorBanner {
                            visible: draft.hasDestination && !draft.hostIsBluetooth && page.catalogBroken
                            Layout.fillWidth: true
                            text: qsTr("Couldn’t load types.")
                            detail: App.emulateError
                            showRetry: true
                            onRetryRequested: page.refreshCatalog()
                        }

                        Repeater {
                            model: draft.hasDestination && !draft.hostIsBluetooth
                                   && !page.catalogLoading && !page.catalogBroken ? page.types : []

                            delegate: Kit.SelectRow {
                                id: typeOption

                                required property var modelData

                                readonly property var preview: App.typeFeatureSummary(draft.hostId,
                                                                                      typeOption.modelData.type)

                                Layout.fillWidth: true
                                selected: draft.type === typeOption.modelData.type
                                title: typeOption.modelData.name
                                subtitle: typeOption.modelData.description

                                onPicked: draft.chooseType(typeOption.modelData.type,
                                                           typeOption.modelData.name)

                                // What the TYPE layer alone offers.
                                Flow {
                                    spacing: Tokens.s2

                                    Repeater {
                                        model: typeOption.preview

                                        delegate: Kit.CapabilityChip {
                                            id: previewChip
                                            required property var modelData

                                            readonly property bool interesting:
                                                previewChip.modelData.feature === "motion"
                                                || previewChip.modelData.feature === "touchpad"
                                                || previewChip.modelData.feature === "rumble"
                                                || previewChip.modelData.feature === "lightbar"

                                            visible: previewChip.interesting
                                            text: draft.featureName(previewChip.modelData.feature)
                                            tone: previewChip.modelData.supported
                                                  ? Kit.CapabilityChip.Present : Kit.CapabilityChip.Absent
                                        }
                                    }
                                }
                            }
                        }

                        // Tune rows. Each is rendered only when every layer carries the
                        // feature — the matrix beside it names the ones that do not, so
                        // a dead control here would be a second, worse explanation.
                        Kit.Card {
                            visible: draft.hasDestination
                                     && (page.motionTunable || page.touchpadTunable
                                         || page.rumbleTunable || page.nothingTunable)
                            Layout.fillWidth: true

                            contentItem: ColumnLayout {
                                spacing: Tokens.s5

                                Kit.LabeledSwitch {
                                    visible: page.motionTunable
                                    Layout.fillWidth: true
                                    label: qsTr("Motion")
                                    description: qsTr("Tilt and gyro aiming on the host.")
                                    checked: draft.motionOn
                                    onToggled: checked => draft.setMotion(checked)
                                }

                                Rectangle {
                                    visible: page.motionTunable && page.touchpadTunable
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: Theme.outlineSubtle
                                }

                                RowLayout {
                                    visible: page.touchpadTunable
                                    Layout.fillWidth: true
                                    spacing: Tokens.s5

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Touchpad")
                                        color: Theme.onSurface
                                        font.pixelSize: Tokens.textBase
                                    }
                                    Kit.SegmentedControl {
                                        small: true
                                        options: page.touchpadOptions
                                        value: page.touchpadOptions[draft.touchpadMode]
                                        onPicked: option => draft.setTouchpad(
                                                      page.touchpadOptions.indexOf(option))
                                    }
                                }

                                Rectangle {
                                    visible: page.touchpadTunable && page.rumbleTunable
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: Theme.outlineSubtle
                                }

                                Kit.LabeledSwitch {
                                    visible: page.rumbleTunable
                                    Layout.fillWidth: true
                                    label: qsTr("Rumble")
                                    description: qsTr("Feedback from the host drives the pad’s motors.")
                                    checked: draft.rumbleOn
                                    onToggled: checked => draft.setRumble(checked)
                                }

                                Label {
                                    visible: page.nothingTunable
                                    Layout.fillWidth: true
                                    text: qsTr("Nothing to tune for this combination — see why on the right. The pad is ready to bind.")
                                    color: Theme.mutedStrong
                                    font.pixelSize: Tokens.textMeta
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }

                    // ══ WHAT CARRIES ════════════════════════════════════════════════
                    // Seven rows at most: it never needs a scroller of its own, and the
                    // page's single scroll region owns all overflow.
                    Kit.Card {
                        Layout.alignment: Qt.AlignTop
                        Layout.fillWidth: page.stacked
                        Layout.minimumWidth: page.matrixMinWidth
                        Layout.preferredWidth: page.matrixWidth

                        contentItem: ColumnLayout {
                            spacing: Tokens.s4

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Tokens.s4

                                Kit.Eyebrow {
                                    Layout.fillWidth: true
                                    text: qsTr("What carries")
                                }
                                Label {
                                    text: page.matrixPending
                                          ? qsTr("pending")
                                          : qsTr("%1/%2 live").arg(page.carriedCount)
                                                              .arg(page.matrixRows.length)
                                    color: Theme.mutedStrong
                                    font.family: Tokens.monoFamily
                                    font.pixelSize: Tokens.textChip
                                }
                            }

                            Kit.CapabilityTable {
                                Layout.fillWidth: true
                                rows: page.matrixRows
                            }

                            Label {
                                Layout.fillWidth: true
                                Layout.topMargin: Tokens.s2
                                text: page.panelFooter
                                color: Theme.mutedStrong
                                font.family: Tokens.monoFamily
                                font.pixelSize: Tokens.textChip
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }
        }

        // ══ ACTION BAR ══════════════════════════════════════════════════════
        // Pinned outside the scroller, divider included; the shell header is
        // fixed above, so only the body between them ever moves.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.outline
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Tokens.s4

            Kit.DishButton {
                visible: page.padBound && !page.noHosts
                variant: Kit.DishButton.Destructive
                text: qsTr("Unbind")
                Accessible.name: qsTr("Unbind %1").arg(page.padName)
                onClicked: page.unbind()
            }

            Label {
                visible: !page.noHosts
                Layout.fillWidth: true
                text: page.actionHint
                color: Theme.mutedStrong
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
            }
            Item {
                visible: page.noHosts
                Layout.fillWidth: true
            }

            Kit.DishButton {
                visible: !page.noHosts
                variant: Kit.DishButton.Outline
                text: qsTr("Cancel")
                onClicked: page.leave()
            }
            Kit.DishButton {
                variant: Kit.DishButton.Primary
                enabled: page.noHosts || page.canApply
                text: page.noHosts ? qsTr("Close")
                                   : page.padBound ? qsTr("Apply") : qsTr("Bind")
                onClicked: page.noHosts ? page.leave() : page.apply()
            }
        }
    }

    // ── Overlays ────────────────────────────────────────────────────────────
    Kit.ConfirmDialog {
        id: discardConfirm
        eyebrow: qsTr("Binding")
        heading: qsTr("Leave without applying?")
        bodyText: qsTr("Nothing has been sent yet. Your changes will be discarded.")
        acceptText: qsTr("Discard")
        rejectText: qsTr("Keep editing")
        destructiveAccept: true

        onAccepted: {
            discardConfirm.close();
            const proceed = page.pendingLeave;
            page.pendingLeave = null;
            if (proceed) {
                proceed();
            }
        }
        onRejected: page.pendingLeave = null
    }

    Kit.ApplyOverlay {
        id: applyOverlay
        steps: page.applySteps
        // Escapable only while the Connection step is live: aborting a Direct
        // claim falls back to Standard, which is a warning, not a failure.
        cancellable: App.applyCancellable
        slowHint: App.applyCancellable && App.applyElapsedMs >= 4000
                  ? qsTr("Handing the device over can take a few seconds.") : ""
        onCancelRequested: App.cancelApply()
    }

    Kit.BlockerDialog {
        id: blocker
        kind: page.padGone ? Kit.BlockerDialog.ControllerUnplugged
                           : Kit.BlockerDialog.ConnectionLost
        hostName: draft.hostName
        onReconnectRequested: {
            blocker.close();
            App.reconnectConnection(draft.hostId);
        }
    }
}
