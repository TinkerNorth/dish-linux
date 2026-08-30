// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Moonlight binding flow's render contract: twenty-one states, one of which
// is drawn at a time, and the guarantee that only ONE of them may stop a user
// from saving a binding. A binding is a durable intent — pairing is remembered
// trust verified lazily, so a host that is unpaired, unreachable, refusing or
// dropped is a state to render and not a reason to refuse the user's answer.
//
// Every case below names the state it walks, so a reordering of the evaluation
// chain cannot quietly send one of them somewhere else.

#include "core/moonlight/MoonlightSessionUi.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dish::moonlight;

namespace {

// Paired, answered, nothing running: the resting shape every case narrows from.
SessionUiInputs paired() {
    SessionUiInputs in;
    in.probeAttempted = true;
    in.probeAnswered = true;
    in.remembered = true;
    in.paired = true;
    in.appsRead = true;
    in.appCount = 2;
    return in;
}

std::string tokenOf(const SessionUiInputs& in) { return sessionUiToken(sessionUiState(in)); }

} // namespace

TEST_CASE("M1 checking: a probe in flight with nothing cached", "[moonlight][ui]") {
    SessionUiInputs in;
    in.probeInFlight = true;
    CHECK(sessionUiState(in) == SessionUiState::Checking);
    CHECK(tokenOf(in) == "checking");
}

TEST_CASE("M2 not paired: answered, PairStatus 0, nothing remembered", "[moonlight][ui]") {
    SessionUiInputs in;
    in.probeAttempted = true;
    in.probeAnswered = true;
    in.paired = false;
    in.remembered = false;
    CHECK(sessionUiState(in) == SessionUiState::NotPaired);
    CHECK(tokenOf(in) == "notPaired");
}

TEST_CASE("M2 again: the host's word alone is not a pairing", "[moonlight][ui]") {
    // The disagreement a Forget leaves behind. A host reports PairStatus
    // against the uniqueid on the request, and the client identity outlives a
    // Forget, so a box we forgot still answers 1. Trust is MUTUAL: every
    // paired-only call is mutual TLS pinned against the certificate the
    // handshake verified, and that certificate went with the row. Reporting
    // Paired here would hide the Pair button behind a chip nothing can act on.
    SessionUiInputs in;
    in.probeAttempted = true;
    in.probeAnswered = true;
    in.paired = true;      // the host's half
    in.remembered = false; // ours
    CHECK(sessionUiState(in) == SessionUiState::NotPaired);
    CHECK(hostTrust(in) == HostTrust::NotPaired);
    // Not TrustLost: nothing was lost, and an ordinary pairing recovers it.
    CHECK(tokenOf(in) == "notPaired");
}

TEST_CASE("a rejection only takes away trust there was to lose", "[moonlight][ui]") {
    // A 401 from a host nobody ever paired with refuses in exactly the way a
    // host that dropped us does. Calling that "trust lost" tells a first-time
    // user that a pairing they never made has been removed, which is false, and
    // NotPaired carries the identical next step.
    SessionUiInputs stranger;
    stranger.probeAttempted = true;
    stranger.trustRejected = true;
    stranger.remembered = false;
    CHECK(sessionUiState(stranger) == SessionUiState::NotPaired);
    CHECK(hostTrust(stranger) == HostTrust::NotPaired);

    // With our certificate still on file the rejection really did take
    // something away, and that is the state that says so.
    SessionUiInputs known = stranger;
    known.remembered = true;
    CHECK(sessionUiState(known) == SessionUiState::TrustLost);
    CHECK(hostTrust(known) == HostTrust::NotPaired);

    // And a rejection outranks the "nobody has answered yet" fallback: a host
    // that just refused us must not promise a session when it comes back.
    CHECK_FALSE(sessionUiState(known) == SessionUiState::Remembered);
}

TEST_CASE("a trust problem outranks the app list it caused", "[moonlight][ui]") {
    // The app list is HTTPS and paired-only, so a host we cannot open a channel
    // to fails it BY CONSTRUCTION. Rendering "could not read the app list" over
    // that puts a transport complaint where the answer is Pair, and it is the
    // one sentence the user has no way to act on.
    SessionUiInputs noCert;
    noCert.probeAttempted = true;
    noCert.probeAnswered = true;
    noCert.paired = true;      // the host's word
    noCert.remembered = false; // and nothing of ours to pin against
    noCert.appsFailed = true;
    CHECK(sessionUiState(noCert) == SessionUiState::NotPaired);

    // Same once the mutual-TLS call comes back 401, which is what that failure
    // actually was.
    SessionUiInputs rejected = noCert;
    rejected.trustRejected = true;
    CHECK(sessionUiState(rejected) == SessionUiState::NotPaired);

    // And with our certificate on file the same 401 is the loss it looks like.
    SessionUiInputs lost = rejected;
    lost.remembered = true;
    CHECK(sessionUiState(lost) == SessionUiState::TrustLost);
}

TEST_CASE("the chip and the sentence under it read one rule", "[moonlight][ui]") {
    // Two functions answering the same question differently is the shape of the
    // live dead end: the row read the host's word alone, said Paired, and hid
    // the Pair button, while the section below it could not open a channel at
    // all. Walked as a cross product so an edit to one has to move the other.
    for (const bool probeAnswered : {false, true}) {
        for (const bool paired : {false, true}) {
            for (const bool remembered : {false, true}) {
                for (const bool trustRejected : {false, true}) {
                    SessionUiInputs in;
                    in.probeAttempted = true;
                    in.probeAnswered = probeAnswered;
                    in.paired = paired;
                    in.remembered = remembered;
                    in.trustRejected = trustRejected;
                    CAPTURE(probeAnswered, paired, remembered, trustRejected);

                    const SessionUiState state = sessionUiState(in);
                    const bool sectionUnpaired =
                        state == SessionUiState::NotPaired || state == SessionUiState::TrustLost;

                    // Where the section says the user is not paired, the row
                    // offers the way back rather than a memory it cannot use.
                    if (sectionUnpaired) { CHECK(hostTrust(in) == HostTrust::NotPaired); }
                    // And the converse, which is the half that stranded the
                    // user: Paired is the one chip that hides the Pair button,
                    // so it may never sit above a section that disagrees.
                    if (hostTrust(in) == HostTrust::Paired) { CHECK_FALSE(sectionUnpaired); }
                    // The two unpaired states split on ONE thing, in both.
                    if (trustRejected) {
                        CHECK(state ==
                              (remembered ? SessionUiState::TrustLost : SessionUiState::NotPaired));
                        CHECK(hostTrust(in) == HostTrust::NotPaired);
                    }
                }
            }
        }
    }
}

TEST_CASE("M3 pairing: the PIN is on screen", "[moonlight][ui]") {
    SessionUiInputs in;
    in.pairingActive = true;
    CHECK(sessionUiState(in) == SessionUiState::PairingPin);
    // It wins over everything, because a live pairing attempt IS the state.
    SessionUiInputs overlapping = paired();
    overlapping.pairingActive = true;
    CHECK(sessionUiState(overlapping) == SessionUiState::PairingPin);
}

TEST_CASE("M4 pairing refused", "[moonlight][ui]") {
    SessionUiInputs in;
    in.probeAttempted = true;
    in.probeAnswered = true;
    in.pairingRefused = true;
    CHECK(sessionUiState(in) == SessionUiState::PairingRefused);
    CHECK(tokenOf(in) == "pairingRefused");
}

TEST_CASE("M5 and M6 split on what is remembered, not on what happened", "[moonlight][ui]") {
    // Both are "the host did not answer". The difference is whether there is a
    // pairing to come back to, and the copy says something different for each.
    SessionUiInputs never;
    never.probeAttempted = true;
    never.probeInFlight = false;
    never.probeAnswered = false;
    never.remembered = false;
    CHECK(sessionUiState(never) == SessionUiState::Unreachable);

    SessionUiInputs known = never;
    known.remembered = true;
    CHECK(sessionUiState(known) == SessionUiState::Remembered);
    CHECK(tokenOf(known) == "remembered");

    // The same split when the failure came from a session attempt.
    SessionUiInputs failed;
    failed.probeAttempted = true;
    failed.failure = SessionFailure::Unreachable;
    CHECK(sessionUiState(failed) == SessionUiState::Unreachable);
    failed.remembered = true;
    CHECK(sessionUiState(failed) == SessionUiState::Remembered);
}

TEST_CASE("a host nobody has asked yet is checking, never silent", "[moonlight][ui]") {
    // Reporting "not answering" about a host no request has ever gone to would
    // be an accusation the client cannot support.
    SessionUiInputs untouched;
    CHECK(sessionUiState(untouched) == SessionUiState::Checking);
    untouched.remembered = true;
    CHECK(sessionUiState(untouched) == SessionUiState::Checking);
}

TEST_CASE("M7 trust lost: answered unpaired with a certificate stored", "[moonlight][ui]") {
    SessionUiInputs in;
    in.probeAttempted = true;
    in.probeAnswered = true;
    in.paired = false;
    in.remembered = true;
    CHECK(sessionUiState(in) == SessionUiState::TrustLost);
    CHECK(tokenOf(in) == "trustLost");

    // A 401 on a mutual-TLS call says the same thing, whatever the probe said.
    SessionUiInputs rejected = paired();
    rejected.trustRejected = true;
    CHECK(sessionUiState(rejected) == SessionUiState::TrustLost);

    // And the session reducer's own token maps here too.
    SessionUiInputs fromSession;
    fromSession.probeAttempted = true;
    fromSession.probeAnswered = true;
    fromSession.paired = true;
    fromSession.remembered = true;
    fromSession.failure = SessionFailure::TrustLost;
    CHECK(sessionUiState(fromSession) == SessionUiState::TrustLost);
}

TEST_CASE("M8 host replaced: a uniqueid we do not remember", "[moonlight][ui]") {
    SessionUiInputs in = paired();
    in.identityChanged = true;
    CHECK(sessionUiState(in) == SessionUiState::HostReplaced);
    CHECK(tokenOf(in) == "hostReplaced");

    // It is named before pairing is judged: "no longer recognises this device"
    // would be the wrong reason for a machine that was reset.
    SessionUiInputs unpaired;
    unpaired.probeAttempted = true;
    unpaired.probeAnswered = true;
    unpaired.remembered = true;
    unpaired.identityChanged = true;
    CHECK(sessionUiState(unpaired) == SessionUiState::HostReplaced);
}

TEST_CASE("M9 through M12: the app list is a state, not a list", "[moonlight][ui]") {
    SessionUiInputs loading = paired();
    loading.appsRead = false;
    loading.appCount = 0;
    loading.appsInFlight = true;
    CHECK(sessionUiState(loading) == SessionUiState::AppsLoading);

    SessionUiInputs ready = paired();
    CHECK(sessionUiState(ready) == SessionUiState::NewSession);
    CHECK(tokenOf(ready) == "newSession");

    SessionUiInputs empty = paired();
    empty.appCount = 0;
    CHECK(sessionUiState(empty) == SessionUiState::NoApps);

    // FAILED IS NOT EMPTY. The list is HTTPS and paired-only, so a refusal read
    // as an empty list would present a 404 as a fact about the host.
    SessionUiInputs failed = paired();
    failed.appsRead = false;
    failed.appCount = 0;
    failed.appsFailed = true;
    CHECK(sessionUiState(failed) == SessionUiState::AppsFailed);
    CHECK(tokenOf(failed) != "noApps");
}

TEST_CASE("M13 joining: a session of ours is already up", "[moonlight][ui]") {
    SessionUiInputs in = paired();
    in.sessionLive = true;
    in.otherControllers = 1;
    CHECK(sessionUiState(in) == SessionUiState::Joining);
    CHECK(tokenOf(in) == "joining");
    // No app question survives here: whoever created the session settled it.
    in.appsRead = false;
    in.appsFailed = true;
    CHECK(sessionUiState(in) == SessionUiState::Joining);
}

TEST_CASE("M14 host full is the ONE state that blocks", "[moonlight][ui]") {
    SessionUiInputs in = paired();
    in.otherControllers = 4;
    CHECK(sessionUiState(in) == SessionUiState::HostFull);
    CHECK(sessionUiBlocksApply(SessionUiState::HostFull));

    // A live session on a full host is still full: "joining" would invite the
    // user into a session that has no room for them.
    SessionUiInputs live = in;
    live.sessionLive = true;
    CHECK(sessionUiState(live) == SessionUiState::HostFull);

    // And so is a host nobody has managed to reach. The ceiling is local
    // bookkeeping, so no network answer can change it, and rendering a spinner
    // or an unreachable host over it would enable an Apply the bind refuses.
    SessionUiInputs unreachable;
    unreachable.probeAttempted = true;
    unreachable.otherControllers = 4;
    CHECK(sessionUiState(unreachable) == SessionUiState::HostFull);
    SessionUiInputs unasked;
    unasked.otherControllers = 4;
    CHECK(sessionUiState(unasked) == SessionUiState::HostFull);

    // Three others plus this binding is exactly the ceiling, and still fits.
    SessionUiInputs room = paired();
    room.otherControllers = 3;
    room.sessionLive = true;
    CHECK(sessionUiState(room) == SessionUiState::Joining);
}

TEST_CASE("M15 through M18: the refusals a host answers 200 with", "[moonlight][ui]") {
    SessionUiInputs busy = paired();
    busy.failure = SessionFailure::AppAlreadyRunning;
    CHECK(sessionUiState(busy) == SessionUiState::BusyOther);

    SessionUiInputs resume = paired();
    resume.failure = SessionFailure::ResumeFailed;
    CHECK(sessionUiState(resume) == SessionUiState::ResumeFailed);

    SessionUiInputs refused = paired();
    refused.failure = SessionFailure::LaunchRejected;
    CHECK(sessionUiState(refused) == SessionUiState::Refused);

    // The stream never came up. Both roads there read the same, because the
    // user's move is the same and Dish has already cancelled the app.
    for (const SessionFailure setup : {SessionFailure::RtspRejected, SessionFailure::ControlLost}) {
        SessionUiInputs in = paired();
        in.failure = setup;
        CHECK(sessionUiState(in) == SessionUiState::SetupFailed);
    }
}

TEST_CASE("M19 live is this binding's own place in the session", "[moonlight][ui]") {
    SessionUiInputs in = paired();
    in.sessionLive = true;
    in.bindingLive = true;
    in.otherControllers = 2;
    CHECK(sessionUiState(in) == SessionUiState::Live);
    CHECK(tokenOf(in) == "live");
    // Live outranks a stale failure from an earlier attempt.
    in.failure = SessionFailure::Dropped;
    CHECK(sessionUiState(in) == SessionUiState::Live);
}

TEST_CASE("M20 and M21 are never merged", "[moonlight][ui]") {
    // A drop is recoverable and the host will usually let us resume; a session
    // the host ended is not.
    SessionUiInputs dropped = paired();
    dropped.failure = SessionFailure::Dropped;
    CHECK(sessionUiState(dropped) == SessionUiState::Dropped);

    SessionUiInputs ended = paired();
    ended.failure = SessionFailure::HostEnded;
    CHECK(sessionUiState(ended) == SessionUiState::EndedByHost);

    CHECK(std::string(sessionUiToken(SessionUiState::Dropped)) !=
          std::string(sessionUiToken(SessionUiState::EndedByHost)));
}

TEST_CASE("every state has its own token and all twenty-one are reachable", "[moonlight][ui]") {
    const SessionUiState all[] = {
        SessionUiState::Checking,       SessionUiState::NotPaired,    SessionUiState::PairingPin,
        SessionUiState::PairingRefused, SessionUiState::Unreachable,  SessionUiState::Remembered,
        SessionUiState::TrustLost,      SessionUiState::HostReplaced, SessionUiState::AppsLoading,
        SessionUiState::NewSession,     SessionUiState::NoApps,       SessionUiState::AppsFailed,
        SessionUiState::Joining,        SessionUiState::HostFull,     SessionUiState::BusyOther,
        SessionUiState::ResumeFailed,   SessionUiState::Refused,      SessionUiState::SetupFailed,
        SessionUiState::Live,           SessionUiState::Dropped,      SessionUiState::EndedByHost};
    static_assert(sizeof(all) / sizeof(all[0]) == 21, "the render contract is twenty-one states");

    std::string seen;
    for (const SessionUiState state : all) {
        const std::string token = sessionUiToken(state);
        CHECK_FALSE(token.empty());
        // A duplicate token would make two states render as one.
        CHECK(seen.find("|" + token + "|") == std::string::npos);
        seen += "|" + token + "|";
    }
}

TEST_CASE("apply is blocked by exactly one state", "[moonlight][ui]") {
    const SessionUiState all[] = {
        SessionUiState::Checking,       SessionUiState::NotPaired,    SessionUiState::PairingPin,
        SessionUiState::PairingRefused, SessionUiState::Unreachable,  SessionUiState::Remembered,
        SessionUiState::TrustLost,      SessionUiState::HostReplaced, SessionUiState::AppsLoading,
        SessionUiState::NewSession,     SessionUiState::NoApps,       SessionUiState::AppsFailed,
        SessionUiState::Joining,        SessionUiState::BusyOther,    SessionUiState::ResumeFailed,
        SessionUiState::Refused,        SessionUiState::SetupFailed,  SessionUiState::Live,
        SessionUiState::Dropped,        SessionUiState::EndedByHost};
    for (const SessionUiState state : all) { CHECK_FALSE(sessionUiBlocksApply(state)); }
    CHECK(sessionUiBlocksApply(SessionUiState::HostFull));
}

TEST_CASE("the host row says trust, and never liveness", "[moonlight][ui]") {
    SessionUiInputs verified = paired();
    CHECK(hostTrust(verified) == HostTrust::Paired);
    CHECK(std::string(hostTrustToken(HostTrust::Paired)) == "paired");

    // Did not answer this visit, but the pairing is stored: remembered, and
    // neutral rather than amber. It is not a problem, only unconfirmed.
    SessionUiInputs offline;
    offline.remembered = true;
    CHECK(hostTrust(offline) == HostTrust::Remembered);

    // Answered and unpaired is a fact about now, whatever is remembered.
    SessionUiInputs unpaired;
    unpaired.probeAttempted = true;
    unpaired.probeAnswered = true;
    unpaired.remembered = true;
    CHECK(hostTrust(unpaired) == HostTrust::NotPaired);

    // A 401 and a changed identity both drop trust outright.
    SessionUiInputs rejected = paired();
    rejected.trustRejected = true;
    CHECK(hostTrust(rejected) == HostTrust::NotPaired);
    SessionUiInputs replaced = paired();
    replaced.identityChanged = true;
    CHECK(hostTrust(replaced) == HostTrust::NotPaired);

    // Never asked at all.
    CHECK(hostTrust(SessionUiInputs{}) == HostTrust::NotPaired);
}

TEST_CASE("the host phase distinguishes what four tokens cannot", "[moonlight][ui]") {
    SessionState session;
    // Nothing has ever run: a paired host rests at paired, an unpaired one idle.
    CHECK(hostPhaseFor(session, /*paired=*/true, /*everStarted=*/false) == HostPhase::Paired);
    CHECK(hostPhaseFor(session, false, false) == HostPhase::Idle);
    // Something ran and stopped, which is not the same as never having run.
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Closed);

    session.phase = SessionPhase::CheckingInfo;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Launching);
    session.phase = SessionPhase::Launching;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Launching);
    session.phase = SessionPhase::Rtsp;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Connecting);
    session.phase = SessionPhase::ControlConnecting;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Connecting);
    session.phase = SessionPhase::Streaming;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Streaming);

    session.phase = SessionPhase::Failed;
    session.failure = SessionFailure::Dropped;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Faltering);
    session.failure = SessionFailure::LaunchRejected;
    CHECK(hostPhaseFor(session, true, true) == HostPhase::Failed);

    CHECK(std::string(hostPhaseToken(HostPhase::Streaming)) == "streaming");
    CHECK(std::string(hostPhaseToken(HostPhase::Faltering)) == "faltering");
    CHECK(std::string(hostPhaseToken(HostPhase::Closed)) == "closed");
}
