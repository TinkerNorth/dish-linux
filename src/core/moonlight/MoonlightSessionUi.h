// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The render contract for the Moonlight section of the binding flow: one pure,
// total function from what is known about a host to exactly one of twenty-one
// states, plus the lowercase tokens QML localizes. The C++ never vends a
// sentence, the same rule the capability solver and the link vocabulary follow.
//
// PAIRING IS NOT A CONNECTION. Moonlight has no bidirectional liveness: pairing
// is one-time trust, checkable only client-initiated (/serverinfo PairStatus, or
// a mutual-TLS handshake that succeeds, which is itself proof). A host never
// notifies the client, and a host-side unpair is discovered on the next call.
// So trust is REMEMBERED and VERIFIED LAZILY — on entering a screen and before
// starting a session, never polled — and a Moonlight host never draws a live
// connection light.
//
// Two protocol facts this ordering depends on:
//   * `currentgame` and `state` describe OUR session, not the host's. A plain
//     probe always reports free, and a session another device holds is
//     discovered only by attempting /launch. That is why NewSession says "new
//     session" and never "the host is idle".
//   * /cancel answers 200 whether or not anything was running, so a successful
//     cancel proves nothing and the caller must re-probe.

#pragma once

#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"

#include <cstdint>
#include <optional>

namespace dish::moonlight {

// Declaration order is the evaluation order: the first state whose trigger
// holds is the one that renders.
enum class SessionUiState : std::uint8_t {
    Checking,       // probe in flight, nothing cached
    NotPaired,      // answered, PairStatus 0, no stored server certificate
    PairingPin,     // a pairing attempt is live and the PIN is on screen
    PairingRefused, // the pairing attempt finished not-ok
    Unreachable,    // never answered, and nothing remembered
    Remembered,     // never answered, but the pairing is remembered
    TrustLost,      // answered unpaired with a certificate stored, or a 401
    HostReplaced,   // the uniqueid differs from the remembered one
    AppsLoading,    // paired, no session of ours, /applist in flight
    NewSession,     // paired, no session of ours, the list is readable
    NoApps,         // the list came back empty
    AppsFailed,     // /applist failed while paired
    Joining,        // this device already holds a session on this host
    HostFull,       // four controllers already ride this host
    BusyOther,      // refused: an app is running for someone else, no resume
    ResumeFailed,   // resume was offered, then would not hand the session back
    Refused,        // refused for a reason of the host's own
    SetupFailed,    // the app started but the stream never came up
    Live,           // this binding is on a connected control stream
    Dropped,        // was live, the link closed without a host termination
    EndedByHost,    // the host terminated, or the app closed
};

// What the surfaces know about one host at render time. Every field is
// something the client observed; nothing here is inferred.
struct SessionUiInputs {
    bool probeInFlight = false;
    // The host has been asked at least once. False is "we have not asked yet",
    // which is a spinner and not a verdict: nothing may report a host silent
    // before anybody said a word to it.
    bool probeAttempted = false;
    // A /serverinfo answer from THIS visit. False, with a probe attempted and
    // none in flight, is the honest "we asked and nothing came back".
    bool probeAnswered = false;
    // A server certificate is stored, so the pairing is remembered.
    bool remembered = false;
    // Verified this visit: PairStatus 1, or a mutual-TLS call that succeeded.
    bool paired = false;
    bool identityChanged = false;
    bool trustRejected = false;

    bool pairingActive = false;
    bool pairingRefused = false;

    bool appsInFlight = false;
    bool appsRead = false;
    bool appsFailed = false;
    int appCount = 0;

    // The host carries a session of ours, and whether THIS binding is in it.
    bool sessionLive = false;
    bool bindingLive = false;
    // Controllers already riding this host, this binding excluded.
    int otherControllers = 0;

    std::optional<SessionFailure> failure;
};

namespace detail {

// Which of the two "you are not paired" states an input is in. TrustLost
// requires something to have been LOST, which means our certificate is still on
// file: a host we never paired with refuses exactly the way a host that dropped
// us does, and telling a first-time user that a pairing they never made has
// been removed is simply false. NotPaired is the truth and carries the same
// recovery, so the split is on what we hold and on nothing else.
inline SessionUiState unpairedState(bool remembered) {
    return remembered ? SessionUiState::TrustLost : SessionUiState::NotPaired;
}

inline SessionUiState failureState(SessionFailure failure, bool remembered) {
    switch (failure) {
    case SessionFailure::Unreachable:
        return remembered ? SessionUiState::Remembered : SessionUiState::Unreachable;
    case SessionFailure::NotPaired:
        return unpairedState(remembered);
    case SessionFailure::TrustLost:
        return SessionUiState::TrustLost;
    case SessionFailure::HostReplaced:
        return SessionUiState::HostReplaced;
    case SessionFailure::AppAlreadyRunning:
        return SessionUiState::BusyOther;
    case SessionFailure::ResumeFailed:
        return SessionUiState::ResumeFailed;
    case SessionFailure::LaunchRejected:
        return SessionUiState::Refused;
    case SessionFailure::RtspRejected:
    case SessionFailure::ControlLost:
        return SessionUiState::SetupFailed;
    case SessionFailure::Dropped:
        return SessionUiState::Dropped;
    case SessionFailure::HostEnded:
    default:
        return SessionUiState::EndedByHost;
    }
}

// TRUST IS MUTUAL AND THIS CLIENT HOLDS ONE HALF OF IT. A host reports
// PairStatus against the uniqueid on the request, and this install's uniqueid
// outlives a Forget, so a box that still has us on file answers 1 to a client
// that threw its half away. That is the host's word only: every paired-only
// call is mutual TLS pinned against the certificate the pairing handshake
// verified, and with no certificate there is nothing to pin, no app list and no
// session. A host we cannot open a channel to is NOT PAIRED however warmly it
// answers, and the way back in is the same PIN a stranger needs.
//
// A REJECTION SETTLES IT whatever else is known. A 401, or a session the host
// refused as unknown, is the host saying so in as many words, and it outranks
// the "nobody has answered yet" fallback: a host that just refused us must
// never render Remembered, which promises a session it is not going to give.
//
// This is a FUNCTION and not two copies of an expression because the host row
// and the session section answering the same question differently is exactly
// what stranded the user: the row read the host's word alone, said Paired, and
// hid the Pair button, while the section below it could not open a channel.
// Two spellings of one rule drifted once and must not be able to again.
inline bool notPaired(const SessionUiInputs& in) {
    return in.trustRejected || (in.probeAnswered && !(in.paired && in.remembered));
}

} // namespace detail

// Pure and total. Evaluated top to bottom in the declaration order above; the
// two triggers that would otherwise overlap are made precise rather than
// reordered: Joining requires room for this controller, so a host already
// carrying four pads reads HostFull and not an invitation to join it.
inline SessionUiState sessionUiState(const SessionUiInputs& in) {
    if (in.pairingActive) { return SessionUiState::PairingPin; }
    if (in.pairingRefused) { return SessionUiState::PairingRefused; }
    if (in.identityChanged) { return SessionUiState::HostReplaced; }

    // The full host is judged FIRST, before anything the network could change,
    // because it is the one state derived entirely from local bookkeeping and
    // the one state that blocks Apply. Rendering a spinner or an unreachable
    // host over it would enable an Apply the bind is going to refuse.
    const bool hasRoom = in.otherControllers < kMaxPads;
    if (!hasRoom) { return SessionUiState::HostFull; }

    // Judged before the fallback below, because a rejection IS an answer and
    // the two arms are disjoint on probeAnswered anyway. See detail::notPaired.
    if (detail::notPaired(in)) { return detail::unpairedState(in.remembered); }

    if (!in.probeAnswered && !in.bindingLive && !in.sessionLive) {
        if (in.probeInFlight || !in.probeAttempted) { return SessionUiState::Checking; }
        if (in.failure) { return detail::failureState(*in.failure, in.remembered); }
        return in.remembered ? SessionUiState::Remembered : SessionUiState::Unreachable;
    }

    if (in.bindingLive) { return SessionUiState::Live; }
    if (in.failure) { return detail::failureState(*in.failure, in.remembered); }

    if (in.sessionLive) { return SessionUiState::Joining; }

    if (in.appsInFlight) { return SessionUiState::AppsLoading; }
    if (in.appsFailed) { return SessionUiState::AppsFailed; }
    if (in.appsRead && in.appCount == 0) { return SessionUiState::NoApps; }
    return SessionUiState::NewSession;
}

// Apply is never blocked by Moonlight host state: a binding is a durable intent
// and the session is attempted when the controller is used, not when the
// binding is saved. The one exception is a host already carrying four pads,
// which is a hard protocol limit and says so.
inline bool sessionUiBlocksApply(SessionUiState state) { return state == SessionUiState::HostFull; }

inline const char* sessionUiToken(SessionUiState state) {
    switch (state) {
    case SessionUiState::Checking:
        return "checking";
    case SessionUiState::NotPaired:
        return "notPaired";
    case SessionUiState::PairingPin:
        return "pairingPin";
    case SessionUiState::PairingRefused:
        return "pairingRefused";
    case SessionUiState::Unreachable:
        return "unreachable";
    case SessionUiState::Remembered:
        return "remembered";
    case SessionUiState::TrustLost:
        return "trustLost";
    case SessionUiState::HostReplaced:
        return "hostReplaced";
    case SessionUiState::AppsLoading:
        return "appsLoading";
    case SessionUiState::NewSession:
        return "newSession";
    case SessionUiState::NoApps:
        return "noApps";
    case SessionUiState::AppsFailed:
        return "appsFailed";
    case SessionUiState::Joining:
        return "joining";
    case SessionUiState::HostFull:
        return "hostFull";
    case SessionUiState::BusyOther:
        return "busyOther";
    case SessionUiState::ResumeFailed:
        return "resumeFailed";
    case SessionUiState::Refused:
        return "refused";
    case SessionUiState::SetupFailed:
        return "setupFailed";
    case SessionUiState::Live:
        return "live";
    case SessionUiState::Dropped:
        return "dropped";
    case SessionUiState::EndedByHost:
    default:
        return "endedByHost";
    }
}

// ── Host-screen vocabulary ───────────────────────────────────────────────────
// Three words, never a liveness light: what the client remembers, and whether
// this visit confirmed it.
enum class HostTrust : std::uint8_t { Paired, Remembered, NotPaired };

inline HostTrust hostTrust(const SessionUiInputs& in) {
    if (in.identityChanged) { return HostTrust::NotPaired; }
    // THE SAME FUNCTION the session section reads, not a second spelling of it.
    // This row has no TrustLost of its own: both unpaired states render here as
    // the one word that offers the way back.
    if (detail::notPaired(in)) { return HostTrust::NotPaired; }
    // Both halves present, which is the only thing that earns the chip that
    // hides the Pair button.
    if (in.paired && in.remembered) { return HostTrust::Paired; }
    // Nobody answered this visit, so the memory is all there is.
    return in.remembered ? HostTrust::Remembered : HostTrust::NotPaired;
}

inline const char* hostTrustToken(HostTrust trust) {
    switch (trust) {
    case HostTrust::Paired:
        return "paired";
    case HostTrust::Remembered:
        return "remembered";
    case HostTrust::NotPaired:
    default:
        return "notPaired";
    }
}

// The phase the row's own chip reads, converged with dish-windows so the two
// Qt clients speak one vocabulary. It distinguishes the states the section-4
// list needs, which a four-token idle/linking/live/failed ladder cannot.
enum class HostPhase : std::uint8_t {
    Idle,
    Pairing,
    Paired,
    Launching,
    Connecting,
    Streaming,
    Faltering,
    Closed,
    Failed,
};

inline const char* hostPhaseToken(HostPhase phase) {
    switch (phase) {
    case HostPhase::Pairing:
        return "pairing";
    case HostPhase::Paired:
        return "paired";
    case HostPhase::Launching:
        return "launching";
    case HostPhase::Connecting:
        return "connecting";
    case HostPhase::Streaming:
        return "streaming";
    case HostPhase::Faltering:
        return "faltering";
    case HostPhase::Closed:
        return "closed";
    case HostPhase::Failed:
        return "failed";
    case HostPhase::Idle:
    default:
        return "idle";
    }
}

// The session reducer's phase as a host-row phase. Pairing and the remembered
// resting states are the manager's to add; this is the live-session half.
inline HostPhase hostPhaseFor(const SessionState& session, bool paired, bool everStarted) {
    switch (session.phase) {
    case SessionPhase::CheckingInfo:
    case SessionPhase::Launching:
        return HostPhase::Launching;
    case SessionPhase::Rtsp:
    case SessionPhase::ControlConnecting:
        return HostPhase::Connecting;
    case SessionPhase::Streaming:
        return HostPhase::Streaming;
    case SessionPhase::Failed:
        return session.failure == SessionFailure::Dropped ? HostPhase::Faltering
                                                          : HostPhase::Failed;
    case SessionPhase::Idle:
    default:
        break;
    }
    if (everStarted) { return HostPhase::Closed; }
    return paired ? HostPhase::Paired : HostPhase::Idle;
}

} // namespace dish::moonlight
