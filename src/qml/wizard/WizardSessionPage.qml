// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wizard page 4 — Binding · session. Shown only for a Moonlight destination,
// where a binding is not just a wire but a place in somebody's session: the
// first controller on a host decides what runs, every later one joins it.
//
// This page renders exactly ONE of the twenty-one states the C++ derives, and
// its `canAdvance` is ALWAYS true. A binding is a durable intent: pairing is
// remembered trust verified lazily, so the session is attempted when the
// controller is used and never when the binding is saved. Nothing about the
// host may stop the user from saving what they asked for. The single exception
// is a host already carrying four controllers, which is a hard protocol limit
// and says so in its own words.
//
// The app picker appears for the binding that CREATES the session and for no
// other. A binding that joins one shows what is running; it is never offered a
// disabled picker, because a disabled picker implies a choice that does not
// exist.

// Bound: the app-row delegate reads the outer `page` id alongside its modelData.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit

ColumnLayout {
    id: page

    property BindingDraft draft
    // Handed down by the container (the wizard, or Configure binding): a
    // StackView attached property does not reach a page nested inside the
    // step host, and the one state that blocks needs a way off this screen.
    property var shellApi: null

    // ── The wizard's step contract ──────────────────────────────────────────
    // Never blocked, except by the four-controller ceiling.
    readonly property bool canAdvance: !page.blocked
    readonly property string primaryLabel: qsTr("Continue ›")
    readonly property string hint: page.blocked
        ? qsTr("Unbind a controller on %1 to make room.").arg(page.hostName)
        : ""

    function primaryActivated() {
        return true;
    }

    function activated() {
        page.refresh();
        if (!page.draft.hasDestination || !page.draft.hostIsMoonlight)
            return;
        // Re-verify on entering: trust is remembered, not watched, and the app
        // list is only readable once the host answers.
        App.probeMoonlightHost(page.draft.hostId);
        App.refreshMoonlightApps(page.draft.hostId);
    }

    // ── State ───────────────────────────────────────────────────────────────
    // A call is not a binding dependency, so the map is republished on every
    // Moonlight move and read as a plain property below.
    property var session: ({})
    property var appRows: []

    readonly property string phase: page.session.state !== undefined ? page.session.state : ""
    readonly property bool blocked: page.session.blocksApply === true
    readonly property string hostName: page.session.hostName !== undefined
                                       && page.session.hostName.length > 0
                                       ? page.session.hostName : page.draft.hostName
    readonly property string appName: page.session.appName !== undefined
                                      ? page.session.appName : ""
    readonly property int controllerNumber: page.session.controllerNumber !== undefined
                                            ? page.session.controllerNumber : 0
    // Verbatim from the host, because a host refuses for reasons of its own and
    // phrases them itself.
    readonly property string refusal: page.session.refusal !== undefined
                                      ? page.session.refusal : ""
    // Which refusal PairingRefused was. One state, several next steps.
    readonly property string pairingReason: page.session.pairingReason !== undefined
                                            ? page.session.pairingReason : ""

    function refresh() {
        if (!page.draft.hasDestination || !page.draft.hostIsMoonlight) {
            page.session = ({});
            page.appRows = [];
            return;
        }
        page.session = App.moonlightSession(page.draft.hostId, page.draft.slotId);
        page.appRows = App.moonlightApps(page.draft.hostId);
    }

    spacing: Tokens.s6

    Connections {
        target: App
        function onMoonlightChanged() { page.refresh(); }
    }

    // ── Actions ─────────────────────────────────────────────────────────────
    function pairNow() {
        App.pairMoonlight(page.draft.hostId);
    }
    function cancelPairing() {
        App.cancelMoonlightPairing();
    }
    function retry() {
        App.probeMoonlightHost(page.draft.hostId);
        App.refreshMoonlightApps(page.draft.hostId);
    }
    // /cancel answers 200 whether or not anything was running, so success
    // proves nothing: the C++ re-probes and this section re-renders.
    function quitApp() {
        App.quitMoonlightApp(page.draft.hostId);
    }
    function startSession() {
        if (page.draft.hasInput)
            App.bindMoonlight(page.draft.slotId, page.draft.hostId);
        page.retry();
    }
    function pickApp(id, title) {
        page.draft.chooseApp(id, title);
        App.setMoonlightApp(page.draft.hostId, id, title);
    }
    // The one state with nowhere to go inside this flow: the room has to be
    // made on the Controllers board, so send the user there.
    function seeBindings() {
        const api = page.shellApi;
        if (api)
            api.requestNavigation(function () { api.selectDestination(1); });
    }

    // ── Head ────────────────────────────────────────────────────────────────
    Kit.Eyebrow {
        mutedTone: true
        text: qsTr("Session")
    }

    Label {
        // The banner and the empty state carry their own title, so the heading
        // stands down rather than saying the same sentence twice on one screen.
        visible: !page.selfTitled()
        text: page.headingText()
        color: Theme.onSurface
        font.pixelSize: Tokens.textStatus
        font.bold: true
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    Label {
        visible: page.bodyText().length > 0
        text: page.bodyText()
        color: Theme.muted
        font.pixelSize: Tokens.textSummary
        lineHeight: 1.5
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // ── Working states: primary, never amber ────────────────────────────────
    Kit.LoadingSpinner {
        visible: page.phase === "checking" || page.phase === "appsLoading"
        running: visible
        text: page.phase === "checking"
              ? qsTr("Checking %1…").arg(page.hostName)
              : qsTr("Reading the app list from %1…").arg(page.hostName)
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s5
    }

    // ── Live ────────────────────────────────────────────────────────────────
    RowLayout {
        visible: page.phase === "live"
        Layout.fillWidth: true
        spacing: Tokens.s4

        Kit.LiveStat {
            live: true
            text: page.liveBody()
            Layout.fillWidth: true
        }
        Kit.CapabilityChip {
            text: qsTr("Streaming")
            tone: Kit.CapabilityChip.Ok
        }
    }

    // ── Failures that are the host's own words ──────────────────────────────
    Kit.ErrorBanner {
        visible: page.phase === "appsFailed" || page.phase === "refused"
                 || page.phase === "setupFailed"
        Layout.fillWidth: true
        tone: page.phase === "appsFailed" ? Kit.ErrorBanner.Warning : Kit.ErrorBanner.Error
        text: page.phase === "appsFailed"
              ? qsTr("Could not read the app list from %1").arg(page.hostName)
              : page.phase === "setupFailed"
                ? qsTr("Could not finish the session on %1").arg(page.hostName)
                : page.refusal.length > 0
                  ? qsTr("%1 refused the session: %2").arg(page.hostName).arg(page.refusal)
                  : qsTr("%1 refused the session").arg(page.hostName)
        detail: page.phase === "appsFailed"
                ? qsTr("Dish will start whatever the host lists first. Retry once %1 is reachable.")
                    .arg(page.hostName)
                : page.phase === "setupFailed"
                  ? qsTr("The app started but the stream did not come up, so Dish closed it again.")
                  : qsTr("Add the controller anyway and Dish will try again the next time you use it.")
        showRetry: true
        onRetryRequested: page.retry()
    }

    // ── Empty: a state with the next step in it ─────────────────────────────
    Kit.EmptyState {
        visible: page.phase === "noApps"
        Layout.fillWidth: true
        Layout.topMargin: Tokens.s5
        glyph: "dish-off"
        title: qsTr("No apps on this host")
        body: qsTr("%1 has no apps set up yet. Add one on the host, or add the controller and Dish will start whatever the host lists first.")
                .arg(page.hostName)
        actionText: qsTr("Retry")
        showAction: true
        onActionRequested: page.retry()
    }

    // ── Everything with an action attached ──────────────────────────────────
    // Never modal: a flapping session would raise a dialog the user cannot
    // outrun, and every one of these states still allows Apply.
    Kit.Callout {
        visible: page.calloutVisible()
        Layout.fillWidth: true
        tone: page.calloutTone()
        text: page.calloutText()

        Kit.DishButton {
            visible: page.phase === "notPaired"
            text: qsTr("Pair now")
            variant: Kit.DishButton.Primary
            size: Kit.DishButton.Small
            onClicked: page.pairNow()
        }
        Kit.DishButton {
            visible: page.phase === "trustLost" || page.phase === "hostReplaced"
            text: qsTr("Pair again")
            variant: Kit.DishButton.Primary
            size: Kit.DishButton.Small
            onClicked: page.pairNow()
        }
        Kit.DishButton {
            visible: page.phase === "pairingRefused"
            text: qsTr("Try again")
            variant: Kit.DishButton.Primary
            size: Kit.DishButton.Small
            onClicked: page.pairNow()
        }
        Kit.DishButton {
            visible: page.phase === "pairingPin"
            text: qsTr("New code")
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            onClicked: page.pairNow()
        }
        Kit.DishButton {
            visible: page.phase === "pairingPin"
            text: qsTr("Cancel")
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            onClicked: page.cancelPairing()
        }
        // The only destructive action in this flow, and it names the host it
        // will close an app on.
        Kit.DishButton {
            visible: page.phase === "busyOther" || page.phase === "resumeFailed"
                     || page.phase === "live"
            text: qsTr("Close the app on %1").arg(page.hostName)
            variant: Kit.DishButton.Destructive
            size: Kit.DishButton.Small
            onClicked: page.quitApp()
        }
        Kit.DishButton {
            visible: page.phase === "unreachable" || page.phase === "remembered"
                     || page.phase === "busyOther" || page.phase === "resumeFailed"
            text: qsTr("Retry")
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            onClicked: page.retry()
        }
        Kit.DishButton {
            visible: page.phase === "dropped"
            text: qsTr("Reconnect")
            variant: Kit.DishButton.Primary
            size: Kit.DishButton.Small
            onClicked: page.startSession()
        }
        Kit.DishButton {
            visible: page.phase === "endedByHost"
            text: qsTr("Start a session")
            variant: Kit.DishButton.Primary
            size: Kit.DishButton.Small
            onClicked: page.startSession()
        }
        // The only state that blocks Apply, so the way out is the only action.
        Kit.DishButton {
            visible: page.phase === "hostFull"
            text: qsTr("See controllers on %1").arg(page.hostName)
            variant: Kit.DishButton.Outline
            size: Kit.DishButton.Small
            onClicked: page.seeBindings()
        }
    }

    // ── We create the session: one row per app ──────────────────────────────
    Repeater {
        model: page.phase === "newSession" ? page.appRows : []

        delegate: Kit.SelectRow {
            id: appRow
            required property var modelData

            Layout.fillWidth: true
            selected: page.draft.appId === appRow.modelData.id
            title: appRow.modelData.title.length > 0 ? appRow.modelData.title
                                                     : appRow.modelData.id

            onPicked: page.pickApp(appRow.modelData.id, appRow.modelData.title)
        }
    }

    Label {
        visible: page.phase === "newSession" && page.draft.appId.length === 0
        text: qsTr("Without a pick, Dish starts whatever %1 lists first.").arg(page.hostName)
        color: Theme.mutedStrong
        font.pixelSize: Tokens.textMeta
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    Item {
        Layout.fillHeight: true
        Layout.minimumHeight: Tokens.s5
    }

    // ── Copy: one English string per state, and the C++ names the state ─────

    // States whose own component already draws the sentence, so the heading
    // stands down rather than printing it a second time above.
    function selfTitled() {
        return page.phase === "noApps" || page.phase === "appsFailed"
               || page.phase === "refused" || page.phase === "setupFailed"
               || page.phase === "checking" || page.phase === "appsLoading";
    }

    function headingText() {
        switch (page.phase) {
        case "checking":       return qsTr("Checking %1…").arg(page.hostName);
        case "notPaired":      return qsTr("Not paired yet");
        case "pairingPin":     return qsTr("Pair with %1").arg(page.hostName);
        case "pairingRefused": return qsTr("%1 did not accept the PIN").arg(page.hostName);
        case "unreachable":
        case "remembered":     return qsTr("%1 is not answering").arg(page.hostName);
        case "trustLost":      return qsTr("%1 no longer recognises this device")
                                        .arg(page.hostName);
        case "hostReplaced":   return qsTr("%1 was reset").arg(page.hostName);
        case "appsLoading":    return qsTr("Reading the app list from %1…").arg(page.hostName);
        case "newSession":     return qsTr("New session");
        case "noApps":         return qsTr("No apps on this host");
        case "appsFailed":     return qsTr("Could not read the app list from %1")
                                        .arg(page.hostName);
        case "joining":        return page.appName.length > 0
                                      ? qsTr("Joining %1").arg(page.appName)
                                      : qsTr("Joining the session on %1").arg(page.hostName);
        case "hostFull":       return qsTr("%1 is full").arg(page.hostName);
        case "busyOther":      return qsTr("Another device is using %1").arg(page.hostName);
        case "resumeFailed":   return qsTr("Could not rejoin the session on %1").arg(page.hostName);
        case "refused":        return page.refusal.length > 0
                                      ? qsTr("%1 refused the session: %2")
                                          .arg(page.hostName).arg(page.refusal)
                                      : qsTr("%1 refused the session").arg(page.hostName);
        case "setupFailed":    return qsTr("Could not finish the session on %1").arg(page.hostName);
        case "live":           return qsTr("Streaming to %1").arg(page.hostName);
        case "dropped":        return qsTr("Session on %1 ended").arg(page.hostName);
        case "endedByHost":    return qsTr("%1 ended the session").arg(page.hostName);
        }
        return qsTr("Session");
    }

    function bodyText() {
        switch (page.phase) {
        case "notPaired":
            return qsTr("%1 needs a one time PIN before Dish can start a session. Pair now, or add the controller and pair later.")
                     .arg(page.hostName);
        case "pairingPin":
            return qsTr("Type %1 into the Moonlight or Sunshine page on %2.")
                     .arg(App.moonlightPairingPin).arg(page.hostName);
        case "pairingRefused":
            return page.pairFailedText();
        case "unreachable":
            return qsTr("Check that the host is switched on and on this network, then try again.");
        case "remembered":
            return qsTr("Dish remembers the pairing with %1 and will start a session when the host is back.")
                     .arg(page.hostName);
        case "trustLost":
            return qsTr("The host removed the pairing. Pair again to start a session.");
        case "hostReplaced":
            return qsTr("This host has a new identity, so the old pairing no longer works. Pair again to start a session.");
        case "newSession":
            return qsTr("This is the first controller on %1, so it picks what the host runs.")
                     .arg(page.hostName);
        case "joining":
            return qsTr("%1 is already running a session for this device. This controller joins it as controller %2.")
                     .arg(page.hostName).arg(page.controllerNumber);
        case "hostFull":
            return qsTr("A session carries four controllers at most, and %1 already has four. Unbind one to make room.")
                     .arg(page.hostName);
        case "busyOther":
            return qsTr("%1 is running an app for a different device and will not hand that session over. Close it to start a new one, or add the controller and try again later.")
                     .arg(page.hostName);
        case "resumeFailed":
            return qsTr("The host has a session but would not hand it back. Close the app on %1 and start a new one.")
                     .arg(page.hostName);
        case "dropped":
            return qsTr("The link dropped. Dish will rejoin the next time you use this controller.");
        case "endedByHost":
            return qsTr("The app closed on the host. Start a new session to keep using this controller.");
        case "live":
            return page.liveBody();
        }
        return "";
    }

    // The same ladder the host screen shows, off the same token: an attempt
    // that never reached the wire must not be reported as a mistyped PIN.
    function pairFailedText() {
        switch (page.pairingReason) {
        case "unreachable":
            return qsTr("%1 did not answer. Check that it is switched on and on this network.")
                     .arg(page.hostName);
        case "declined":
            return qsTr("%1 turned the request down. Check that pairing is allowed on the host.")
                     .arg(page.hostName);
        case "crypto":
            return qsTr("Dish could not prepare its own identity for pairing. Try again.");
        default:
            return qsTr("Check that the code went into the right host, then try again.");
        }
    }

    function liveBody() {
        return qsTr("%1 · controller %2 of 4")
                 .arg(page.appName.length > 0 ? page.appName : page.hostName)
                 .arg(page.controllerNumber);
    }

    function calloutVisible() {
        switch (page.phase) {
        case "notPaired":
        case "pairingPin":
        case "pairingRefused":
        case "unreachable":
        case "remembered":
        case "trustLost":
        case "hostReplaced":
        case "hostFull":
        case "busyOther":
        case "resumeFailed":
        case "dropped":
        case "endedByHost":
        case "live":
            return true;
        }
        return false;
    }

    // Amber is the PROBLEM colour, never the working one: a PIN on screen is
    // information, and a live session is not a warning.
    function calloutTone() {
        switch (page.phase) {
        case "notPaired":
        case "pairingPin":
        case "live":
            return Kit.Callout.Info;
        case "hostFull":
        case "hostReplaced":
        case "trustLost":
            return Kit.Callout.Error;
        }
        return Kit.Callout.Warning;
    }

    function calloutText() {
        if (page.phase === "live")
            return qsTr("Unbinding the last controller ends this session.");
        if (page.phase === "pairingPin")
            return qsTr("Waiting for the host to accept the PIN…");
        // The one state that is genuinely a dead end until something changes;
        // every other one still lets the binding be saved.
        if (page.phase === "hostFull")
            return qsTr("This is the only Moonlight state that stops you adding the controller.");
        // Every other state already said its piece in the body above; the
        // callout carries the actions, and repeating the sentence inside it
        // would say the same thing twice on one screen.
        return qsTr("You can add the controller now and settle this later.");
    }
}
