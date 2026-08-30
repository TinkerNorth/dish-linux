// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The setup wizard — the ONE way a binding is created; a pushed page on Home's
// content stack, never a dialog. Layout law: the banner and the footer are
// PINNED outside a scrolling body, so the primary action stays reachable at the
// 900x620 minimum. Review's single App.applyBinding() carries every answer.

// Bound: the host-row watcher's delegate reads the outer `wizard` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
// BindingDraft comes from Dish.Chrome, NOT from a `import "../shared"` directory
// import: the module copy and the source-tree copy are two distinct types, so a
// directory import here makes `draft: wizard.draft` fail to assign to the sub-
// pages' own `property BindingDraft draft` (qmllint incompatible-type).
import Dish.Chrome
import "../kit" as Kit

Kit.Page {
    id: wizard

    // ── The shell contract ──────────────────────────────────────────────────
    // Set by openSetupWizard(slotId): a dangling pad pre-answers page 1 and the
    // wizard opens on page 2, with Back to page 1 still reachable.
    property string seedSlotId: ""

    // Two controls named Back, 500px apart, is the defect this closes.
    readonly property bool suppressBack: true
    // A rail click REPLACES the content stack. Ask first, but only once the
    // draft holds more than the pre-answered input — a pristine page 1 loses
    // nothing.
    readonly property bool blocksLeave: wizard.step > 0
                                        || wizard.draft.desiredPath === "direct"

    readonly property string headerTitle: qsTr("Set up Dish")
    // "changed", not "written": page 3 really does talk to the host (it reads
    // its type catalog). The wire is what has not been sent.
    readonly property string headerSub: qsTr("Nothing is changed until the last step")
    // A dot that is primary on every frame is decoration, and trains the user to
    // ignore the header.
    readonly property string headerDot: wizard.applied ? "success"
                                      : (wizard.draft.hasInput && wizard.draft.hasDestination)
                                        ? "primary" : "muted"

    // The page owns its own regions; Kit.Page's scroller is not the body's.
    scrollable: false

    readonly property var shellStack: StackView.view
    readonly property var shellApi: wizard.shellStack ? wizard.shellStack.shellApi : null

    // ── Step state ──────────────────────────────────────────────────────────
    // 0 Input · 1 Destination · 2 Type · 3 Session · 4 Feel · 5 Review.
    //
    // Step 3 exists only for a Moonlight destination, where what the host runs
    // is a distinct question from how the pad feels and is the only step that
    // can fail. It is instantiated either way (every step is a sibling gated on
    // `visible`) and simply skipped, so nothing downstream has to know.
    property int step: 0
    readonly property int lastStep: 5
    readonly property bool hasSessionStep: wizard.draft.hostKind === "moonlight"
    // Latched on a successful bind so the header dot and the blockers know the
    // wire is real before the pop lands.
    property bool applied: false
    readonly property bool applying: App.applyInFlight

    // Stage 1 Input, 2 Destination, 3 Binding — three pages live in stage 3,
    // four for a Moonlight binding, which is why it also carries a sub-step
    // indicator. Kit.WizardBanner already renders a variable sub-step count.
    readonly property int stage: wizard.step === 0 ? 1 : wizard.step === 1 ? 2 : 3
    readonly property int subStep: wizard.step >= 2 ? wizard.step - 2 : 0

    // Resolved by call, not by a property binding: onStepChanged fires BEFORE a
    // binding on `step` re-evaluates, so reading `activePage` from the handler
    // yields the page we just left and the entered step never gets activated().
    function pageForStep(n) {
        return n === 0 ? inputPage
             : n === 1 ? destinationPage
             : n === 2 ? typePage
             : n === 3 ? sessionPage
             : n === 4 ? feelPage
             : reviewPage;
    }

    // The next/previous step the CURRENT draft actually has, so a satellite
    // binding never lands on the session page and never has to step past it.
    function stepAfter(n) {
        const next = n + 1;
        return (next === 3 && !wizard.hasSessionStep) ? 4 : next;
    }
    function stepBefore(n) {
        const prev = n - 1;
        return (prev === 3 && !wizard.hasSessionStep) ? 2 : prev;
    }

    readonly property var activePage: wizard.pageForStep(wizard.step)

    // What the adopted pad is, republished by page 1 so the banner and the
    // review card read one snapshot instead of re-walking the slot model.
    property var padInfo: null

    property bool padMissing: false
    // The chosen host has been live at least once this run, and is live now.
    // A host that was never live is not "lost" — it is simply not connected.
    property bool hostWasLive: false
    property bool hostLive: false

    readonly property bool padGone: wizard.draft.hasInput && wizard.padMissing
    readonly property bool hostLost: wizard.hostWasLive && !wizard.hostLive
                                     && !wizard.applied

    // The leave callback the shell (or Cancel) handed us, held while the
    // discard confirm is up.
    property var pendingLeave: null

    readonly property int slowHintAfterMs: 4000

    // ── The draft — one model, and it lives exactly as long as this page ────
    // The id is NOT `draft`: an id shadows a same-named property in its own
    // component scope, and the alias below would then bind to itself.
    BindingDraft {
        id: bindingDraft
        catalogFailed: App.emulateError.length > 0
    }
    readonly property BindingDraft draft: bindingDraft

    // Bumped on every AppModel state move so the invokable-backed readouts (slot
    // accounting, pad presence) re-evaluate: a call is not a binding dependency.
    property int accounting: 0

    // ── Footer copy ─────────────────────────────────────────────────────────
    // The position ALWAYS leads. Three consecutive pages under an unchanging
    // stage marker is 60% of the flow with no progress signal.
    readonly property string stepPositionText:
        wizard.step === 0 ? qsTr("Step 1 of 3 · Input")
      : wizard.step === 1 ? qsTr("Step 2 of 3 · Destination")
      : wizard.step === 2 ? qsTr("Step 3 of 3 · Type")
      : wizard.step === 3 ? qsTr("Step 3 of 3 · Session")
      : wizard.step === 4 ? qsTr("Step 3 of 3 · Feel")
      : qsTr("Step 3 of 3 · Review")

    readonly property string hintText: {
        if (wizard.applying)
            return qsTr("Sending…");
        const advice = wizard.activePage && wizard.activePage.hint ? wizard.activePage.hint : "";
        if (advice.length === 0)
            return wizard.stepPositionText;
        return qsTr("%1 · %2").arg(wizard.stepPositionText).arg(advice);
    }

    readonly property string primaryLabel: wizard.activePage ? wizard.activePage.primaryLabel : ""
    readonly property bool primaryEnabled: !wizard.applying && wizard.activePage
                                           && wizard.activePage.canAdvance

    // ── Banner content ──────────────────────────────────────────────────────

    function padSubText() {
        if (!wizard.padInfo)
            return "";
        const parts = [wizard.padInfo.transport];
        if (!wizard.padInfo.bluetooth && wizard.padInfo.claimable)
            parts.push(wizard.draft.desiredPath === "direct" ? qsTr("Direct") : qsTr("Standard"));
        // The rate joins only from Review onward, where the banner is the
        // review and the numbers are the point.
        if (wizard.step >= wizard.lastStep) {
            const rate = rateFormat.rateText(wizard.padInfo.hz, wizard.padInfo.hzLive);
            if (rate.length > 0)
                parts.push(rate);
        }
        return parts.join(" · ");
    }

    // Never a slot NUMBER before bindSlot allocates one. `accounting` is read
    // so the caller's binding re-runs when a slot is bound elsewhere.
    function hostSubText() {
        const free = wizard.accounting >= 0
                   ? App.hostSlotCapacity() - App.hostBoundSlotCount(wizard.draft.hostId) : 0;
        // A Moonlight session carries the same four, and calling it a satellite
        // would be the one word on this screen that is simply untrue.
        const kind = wizard.draft.hostKind === "moonlight" ? qsTr("moonlight")
                                                           : qsTr("satellite");
        if (free <= 0)
            return qsTr("%1 · 0 slots free").arg(kind);
        return qsTr("%1 · %2").arg(kind).arg(qsTr("%n slots free", "", free));
    }

    readonly property var padSlot: !wizard.draft.hasInput
        ? ({ "title": qsTr("Looking for a pad…"), "sub": qsTr("scanning · usb and bt"),
             "empty": true, "hot": true, "tone": "accent" })
        : ({ "title": wizard.draft.padName, "sub": wizard.padSubText(),
             "empty": false, "hot": wizard.step === 0, "tone": "accent" })

    readonly property var hostSlot: wizard.draft.hasDestination
        // The right slot is the lit one while the destination is the question,
        // and again while the descriptor is in flight.
        ? ({ "title": wizard.draft.hostName, "sub": wizard.hostSubText(),
             "empty": false, "hot": wizard.applying || wizard.step === 1, "tone": "accent" })
        : wizard.step < 1
          ? ({ "title": qsTr("No destination"), "sub": qsTr("step 2"),
               "empty": true, "hot": false, "tone": "accent" })
          : destinationPage.hostCount > 0
            // Seeking is not a problem: amber is reserved for "wrong".
            ? ({ "title": qsTr("%n found", "", destinationPage.hostCount),
                 "sub": qsTr("pick one"), "empty": true, "hot": true, "tone": "accent" })
            : ({ "title": qsTr("Searching…"), "sub": qsTr("%n found", "", 0),
                 "empty": true, "hot": true, "tone": "accent" })

    readonly property string wireLabel: {
        if (wizard.applying)
            return qsTr("binding…");
        if (wizard.step < 2)
            return qsTr("—");
        if (!wizard.draft.hasType || wizard.draft.typeName.length === 0)
            return qsTr("as —");
        if (wizard.step >= wizard.lastStep && reviewPage.extrasSummary.length > 0)
            return qsTr("as %1 · %2").arg(wizard.draft.typeName).arg(reviewPage.extrasSummary);
        return qsTr("as %1").arg(wizard.draft.typeName);
    }

    // ── Navigation ──────────────────────────────────────────────────────────

    function goBack() {
        if (wizard.step > 0 && !wizard.applying)
            wizard.step = wizard.stepBefore(wizard.step);
    }

    function primaryPressed() {
        const page = wizard.activePage;
        if (!page || !page.canAdvance || wizard.applying)
            return;
        // A page that handles its own primary (Destination opening the Pair
        // sheet, Review starting the apply) returns false and does not advance.
        if (page.primaryActivated() === false)
            return;
        if (wizard.step < wizard.lastStep)
            wizard.step = wizard.stepAfter(wizard.step);
    }

    // Completed markers jump back. Back is non-destructive, so this is safe.
    function jumpToStage(target) {
        const targetStep = target === 1 ? 0 : target === 2 ? 1 : 2;
        if (!wizard.applying && targetStep < wizard.step)
            wizard.step = targetStep;
    }

    function cancel() {
        if (wizard.applying)
            return;
        wizard.requestLeave(function () { wizard.leaveNow(); });
    }

    function leaveNow() {
        if (wizard.shellStack)
            wizard.shellStack.pop();
    }

    // The shell's leave guard calls this before replacing the stack.
    function requestLeave(proceed) {
        if (!wizard.blocksLeave) {
            proceed();
            return;
        }
        wizard.pendingLeave = proceed;
        discardConfirm.open();
    }

    function notify(message, severity) {
        // The toast host is the shell's; a second host would queue against
        // itself. Guarded so the wizard still runs in a bare harness.
        if (wizard.shellApi && typeof wizard.shellApi.toast === "function")
            wizard.shellApi.toast(message, severity);
    }

    // ── Apply outcome ───────────────────────────────────────────────────────

    function failureText(reasonToken) {
        if (reasonToken === "slotGone")
            return qsTr("%1 is no longer available. Unplug and reconnect it, then try again.")
                     .arg(wizard.draft.padName);
        if (reasonToken === "hostUnreachable")
            return qsTr("Couldn’t reach %1. Make sure it’s on and on your network.")
                     .arg(wizard.draft.hostName);
        if (reasonToken === "bindRejected")
            return qsTr("%1 refused the binding. Try again.").arg(wizard.draft.hostName);
        // A cancel is the user's own answer; a toast about it is noise.
        return "";
    }

    function onApplyDone(ok, reasonToken, directFellBack) {
        applyOverlay.close();
        if (!ok) {
            // Never auto-close on failure: the user still has a draft.
            const message = wizard.failureText(reasonToken);
            if (message.length > 0)
                wizard.notify(message, "error");
            return;
        }
        wizard.applied = true;
        if (directFellBack) {
            // The binding applied on Standard. A warning, never an error.
            wizard.notify(qsTr("Direct wasn’t available — %1 is bound over Standard.")
                            .arg(wizard.draft.padName), "warning");
        }
        wizard.notify(qsTr("Controller bound — %1 is live on %2.")
                        .arg(wizard.draft.padName).arg(wizard.draft.hostName), "success");
        wizard.leaveNow();
    }

    function refreshPadPresence() {
        wizard.accounting += 1;
        if (!wizard.draft.hasInput) {
            wizard.padMissing = false;
            return;
        }
        // Re-resolved by (vid,pid): a Direct claim swapping the framework slot
        // for its synthetic twin is not a disappearance.
        wizard.padMissing = App.resolveSlotIdForBind(wizard.draft.slotId).length === 0;
    }

    function noteHost(mine, live) {
        if (!mine)
            return;
        wizard.hostLive = live;
        if (live)
            wizard.hostWasLive = true;
    }

    Component.onCompleted: {
        // Page 1's waiting state names the radio as a reason nothing appeared.
        App.refreshBluetoothState();
        // Page 1 is pre-answered by its picker (the only place the pad's name and
        // claimability are known), so the wizard opens on page 2.
        if (wizard.seedSlotId.length > 0)
            wizard.step = 1;
    }

    onStepChanged: {
        const page = wizard.pageForStep(wizard.step);
        if (page && typeof page.activated === "function")
            page.activated();
    }

    onPadGoneChanged: {
        if (wizard.padGone)
            padBlocker.open();
        else
            padBlocker.close();
    }

    onHostLostChanged: {
        if (wizard.hostLost)
            hostBlocker.open();
        else
            hostBlocker.close();
    }

    Connections {
        target: App

        function onStateChanged() { wizard.refreshPadPresence(); }
        function onApplyChanged() {
            if (App.applyInFlight && !applyOverlay.visible)
                applyOverlay.open();
        }
        function onApplyFinished(ok, reasonToken, directFellBack) {
            wizard.onApplyDone(ok, reasonToken, directFellBack);
        }
    }

    // Esc cancels (through the confirm); Alt+Left steps back. Both stand down
    // while a sheet owns the keyboard, so Esc closes the sheet first.
    Shortcut {
        sequence: "Esc"
        enabled: !wizard.sheetOpen
        onActivated: wizard.cancel()
    }
    Shortcut {
        sequence: "Alt+Left"
        enabled: !wizard.sheetOpen
        onActivated: wizard.goBack()
    }

    readonly property bool sheetOpen: applyOverlay.visible || discardConfirm.visible
                                      || padBlocker.visible || hostBlocker.visible
                                      || destinationPage.modalOpen

    // ── The page ────────────────────────────────────────────────────────────
    ColumnLayout {
        id: root
        width: parent.width
        height: parent.height
        spacing: Tokens.s6

        // The one rate formatter; Kit.LiveStat owns the ~ rule.
        Kit.LiveStat {
            id: rateFormat
            visible: false
        }

        // A zero-cost watcher over the connection rows. A Repeater creates every
        // delegate, so the chosen host's link state is tracked on the steps
        // where the destination list is not on screen.
        Item {
            visible: false

            Repeater {
                model: App.connectionModel

                delegate: Item {
                    id: hostWatchRow
                    required property string connectionId
                    required property bool liveLink
                    readonly property bool mine: hostWatchRow.connectionId === wizard.draft.hostId

                    onMineChanged: wizard.noteHost(hostWatchRow.mine, hostWatchRow.liveLink)
                    onLiveLinkChanged: wizard.noteHost(hostWatchRow.mine, hostWatchRow.liveLink)
                    Component.onCompleted: wizard.noteHost(hostWatchRow.mine,
                                                           hostWatchRow.liveLink)
                }
            }
        }

        // ── Pinned banner ───────────────────────────────────────────────────
        Kit.WizardBanner {
            id: banner
            padSlot: wizard.padSlot
            hostSlot: wizard.hostSlot
            wireLabel: wizard.wireLabel
            // The wire is TRANSMITTING while applying, never live: an 8s
            // round-trip that can fail must not draw a connection first.
            live: false
            transmitting: wizard.applying
            stage: wizard.stage
            subStep: wizard.subStep
            subStepCount: wizard.hasSessionStep ? 4 : 3
            // Below four-fifths of the minimum window the banner drops its slot
            // sub-lines and marker labels rather than eating the body.
            compact: root.height < Tokens.minWindowHeight * 0.8
            Layout.fillWidth: true

            onStageClicked: function (clickedStage) { wizard.jumpToStage(clickedStage); }
        }

        // ── Scrolling body ──────────────────────────────────────────────────
        ScrollView {
            id: bodyScroll
            clip: true
            contentWidth: availableWidth
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                id: stepHost
                width: bodyScroll.availableWidth
                // The viewport, unless the step outgrows it — then it scrolls.
                height: Math.max(bodyScroll.availableHeight,
                                 wizard.activePage ? wizard.activePage.implicitHeight : 0)

                WizardInputPage {
                    id: inputPage
                    draft: wizard.draft
                    seedSlotId: wizard.seedSlotId
                    visible: wizard.step === 0
                    width: stepHost.width
                    height: stepHost.height

                    onPadAdopted: function (info) { wizard.padInfo = info; }
                }

                WizardDestinationPage {
                    id: destinationPage
                    draft: wizard.draft
                    visible: wizard.step === 1
                    width: stepHost.width
                    height: stepHost.height

                    onAdvanceRequested: {
                        if (wizard.step === 1)
                            wizard.step = 2;
                    }
                }

                WizardTypePage {
                    id: typePage
                    draft: wizard.draft
                    visible: wizard.step === 2
                    width: stepHost.width
                    height: stepHost.height
                }

                WizardSessionPage {
                    id: sessionPage
                    draft: wizard.draft
                    shellApi: wizard.shellApi
                    visible: wizard.step === 3
                    width: stepHost.width
                    height: stepHost.height
                }

                WizardFeelPage {
                    id: feelPage
                    draft: wizard.draft
                    visible: wizard.step === 4
                    width: stepHost.width
                    height: stepHost.height
                }

                WizardReviewPage {
                    id: reviewPage
                    draft: wizard.draft
                    visible: wizard.step === 5
                    width: stepHost.width
                    height: stepHost.height
                }
            }
        }

        // ── Pinned footer ───────────────────────────────────────────────────
        Rectangle {
            implicitHeight: 1
            color: Theme.outline
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: Tokens.s4
            Layout.fillWidth: true
            Layout.bottomMargin: Tokens.s2

            Kit.DishButton {
                text: qsTr("‹ Back")
                variant: Kit.DishButton.Outline
                // Disabled, never absent: a control that vanishes on page 1
                // moves every other control under the pointer.
                enabled: wizard.step > 0 && !wizard.applying
                onClicked: wizard.goBack()
            }
            Kit.DishButton {
                text: qsTr("Cancel")
                variant: Kit.DishButton.Outline
                enabled: !wizard.applying
                onClicked: wizard.cancel()
            }
            Label {
                text: wizard.hintText
                color: Theme.muted
                font.pixelSize: Tokens.textMeta
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.leftMargin: Tokens.s2
            }
            Kit.DishButton {
                text: wizard.primaryLabel
                variant: Kit.DishButton.Primary
                enabled: wizard.primaryEnabled
                onClicked: wizard.primaryPressed()
            }
        }
    }

    // ── The app-opened sheet family ─────────────────────────────────────────

    Kit.ConfirmDialog {
        id: discardConfirm
        eyebrow: qsTr("Setup")
        heading: qsTr("Leave setup?")
        bodyText: qsTr("Nothing has been sent yet. Your answers will be discarded.")
        acceptText: qsTr("Discard")
        rejectText: qsTr("Keep setting up")
        destructiveAccept: true

        onAccepted: {
            discardConfirm.close();
            const proceed = wizard.pendingLeave;
            wizard.pendingLeave = null;
            if (proceed)
                proceed();
        }
        onRejected: wizard.pendingLeave = null
    }

    Kit.ApplyOverlay {
        id: applyOverlay
        steps: [
            {
                "label": qsTr("Connection"),
                "meta": App.applyConnectionState === "skipped" ? qsTr("standard · kept")
                                                               : qsTr("switching path"),
                // A step that never had to run reads done, not pending: it is
                // settled, and a dimmed row implies something still owes work.
                "state": App.applyConnectionState === "skipped" ? "done"
                                                                : App.applyConnectionState
            },
            {
                "label": qsTr("Destination"),
                "meta": qsTr("sending descriptor"),
                "state": App.applyDestinationState
            }
        ]
        // A Direct claim aborts to Standard, which is a warning. The 8s REST
        // round-trip is short enough to need no escape.
        cancellable: App.applyCancellable
        slowHint: (App.applyConnectionState === "active"
                   && App.applyElapsedMs >= wizard.slowHintAfterMs)
                  ? qsTr("Handing the device over can take a few seconds.") : ""

        onCancelRequested: App.cancelApply()
    }

    Kit.BlockerDialog {
        id: padBlocker
        kind: Kit.BlockerDialog.ControllerUnplugged
    }

    Kit.BlockerDialog {
        id: hostBlocker
        kind: Kit.BlockerDialog.ConnectionLost
        hostName: wizard.draft.hostName

        onReconnectRequested: {
            App.reconnectConnection(wizard.draft.hostId);
            hostBlocker.close();
        }
    }
}
