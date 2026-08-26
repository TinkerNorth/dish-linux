// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Moonlight session-launch lifecycle as a pure, total (state, event) ->
// (state, effects) reducer, modelled on UsbPathMachine. The coordinator
// (source/moonlight/MoonlightSession) turns network completions into events,
// runs reduce() on the Qt main thread, and executes the effects against the
// HTTP, RTSP and ENet edges. Ports and keys are transport data and stay on the
// coordinator; this machine owns only the sequencing.
//
// Happy path:
//   Idle -> CheckingInfo -> Launching -> Rtsp(Options..Play) ->
//   ControlConnecting -> Streaming
// Every failure lands in Failed(reason) with teardown effects; StopRequested
// tears down from any phase and returns to Idle.

#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace dish::moonlight {

enum class SessionPhase : std::uint8_t {
    Idle,
    CheckingInfo,
    Launching,
    Rtsp,
    ControlConnecting,
    Streaming,
    Failed,
};

enum class RtspStep : std::uint8_t {
    Options,
    Describe,
    SetupAudio,
    SetupVideo,
    SetupControl,
    Announce,
    Play,
};

enum class SessionFailure : std::uint8_t {
    Unreachable,       // serverinfo never answered
    NotPaired,         // host answered but this client is not paired
    TrustLost,         // the host answered unpaired, and we remember pairing it
    HostReplaced,      // the host answered with a uniqueid we do not remember
    LaunchRejected,    // launch/resume did not return a session
    AppAlreadyRunning, // the host holds an app and would not hand it over
    ResumeFailed,      // resume was offered, then would not hand the app back
    RtspRejected,      // an RTSP step failed or the TCP transport dropped
    ControlLost,       // ENet connect failed before the link was ever up
    Dropped,           // the LIVE link died; the host will usually resume it
    HostEnded,         // the host sent TERMINATION
};

struct SessionState {
    SessionPhase phase = SessionPhase::Idle;
    RtspStep rtspStep = RtspStep::Options;
    // Set while Launching/later: the launch was a /resume of a running app.
    bool resuming = false;
    std::optional<SessionFailure> failure; // set iff phase == Failed

    bool operator==(const SessionState& o) const {
        return phase == o.phase && rtspStep == o.rtspStep && resuming == o.resuming &&
               failure == o.failure;
    }
    bool operator!=(const SessionState& o) const { return !(*this == o); }
};

// ── Events ────────────────────────────────────────────────────────────────────

namespace moon_event {

struct StartRequested {
    bool operator==(const StartRequested&) const { return true; }
};

// GET /serverinfo answered. `paired` is PairStatus for THIS client;
// `currentGame` non-zero means an app is already running, so the launch phase
// resumes instead. `remembered` is "a server certificate is stored for this
// host", which is what separates a host we never paired with from one that has
// forgotten us; `identityChanged` is a uniqueid that is not the one we
// remember, so the stored pairing anchors nothing any more.
struct ServerInfoOk {
    bool paired = false;
    int currentGame = 0;
    bool remembered = false;
    bool identityChanged = false;
    bool operator==(const ServerInfoOk& o) const {
        return paired == o.paired && currentGame == o.currentGame && remembered == o.remembered &&
               identityChanged == o.identityChanged;
    }
};

struct ServerInfoFailed {
    bool operator==(const ServerInfoFailed&) const { return true; }
};

// /launch or /resume produced an RTSP endpoint.
struct LaunchOk {
    bool operator==(const LaunchOk&) const { return true; }
};

struct LaunchFailed {
    bool operator==(const LaunchFailed&) const { return true; }
};

// The host refused in the BODY: HTTP 200 carrying status_code="400" and "An
// app is already running on this host". `resumable` is its <resume> flag, so a
// launch that may be taken over promotes to /resume instead of failing.
struct LaunchBusy {
    bool resumable = false;
    bool operator==(const LaunchBusy& o) const { return resumable == o.resumable; }
};

// The RTSP TCP transport connected; the machine responds by sending OPTIONS.
struct RtspReady {
    bool operator==(const RtspReady&) const { return true; }
};

// The current RTSP step got a 200.
struct RtspStepOk {
    bool operator==(const RtspStepOk&) const { return true; }
};

// A non-200, a parse failure, or the TCP transport dropped mid-handshake.
struct RtspFailed {
    bool operator==(const RtspFailed&) const { return true; }
};

struct ControlConnected {
    bool operator==(const ControlConnected&) const { return true; }
};

// ENet connect timed out or the established link died.
struct ControlLost {
    bool operator==(const ControlLost&) const { return true; }
};

// The host sent an encrypted TERMINATION message.
struct HostTerminated {
    bool operator==(const HostTerminated&) const { return true; }
};

// User-driven teardown, from any phase.
struct StopRequested {
    bool operator==(const StopRequested&) const { return true; }
};

} // namespace moon_event

using SessionEvent =
    std::variant<moon_event::StartRequested, moon_event::ServerInfoOk, moon_event::ServerInfoFailed,
                 moon_event::LaunchOk, moon_event::LaunchFailed, moon_event::LaunchBusy,
                 moon_event::RtspReady, moon_event::RtspStepOk, moon_event::RtspFailed,
                 moon_event::ControlConnected, moon_event::ControlLost, moon_event::HostTerminated,
                 moon_event::StopRequested>;

// ── Effects (data; the coordinator executes them) ────────────────────────────

enum class SessionEffect : std::uint8_t {
    FetchServerInfo,
    SendLaunch, // /launch, or /resume when state.resuming
    OpenRtsp,   // dial the RTSP TCP endpoint from the launch response
    SendRtspOptions,
    SendRtspDescribe,
    SendRtspSetupAudio,
    SendRtspSetupVideo,
    SendRtspSetupControl,
    SendRtspAnnounce,
    SendRtspPlay,
    ConnectControl,  // ENet connect with the SETUP-provided port + connect data
    StartStreaming,  // arrivals, RTP hole-punch pings, periodic control ping
    SendTermination, // graceful TERMINATION before the disconnect
    Teardown,        // close ENet, RTSP and RTP sockets
    NotifyFailure,   // surface state.failure to the UI
};

// A session is started only from a resting phase. THE SESSION IS PER HOST AND
// REFERENCE COUNTED: a second binding on a host that is already checking,
// launching or live JOINS it and sends only its own CONTROLLER_ARRIVAL, and
// must never launch a second one beside it.
inline bool sessionNeedsStart(SessionPhase phase) {
    return phase == SessionPhase::Idle || phase == SessionPhase::Failed;
}

// Whether a teardown owes the host a /cancel. A launch that never went live
// left the host holding an app on our behalf, and leaving it there gets every
// later attempt refused by our own leftovers. One that DID go live is left
// alone while somebody is still riding it, because closing a running game out
// from under them is worse than the tidying is worth; once the LAST controller
// unbinds there is nobody left to be rude to and the app is handed back.
inline bool shouldHandBackApp(bool launched, bool wentLive, bool lastControllerLeft) {
    return launched && (!wentLive || lastControllerLeft);
}

struct Reduction {
    // nullopt = the event does not apply in this phase; state is unchanged.
    std::optional<SessionState> next;
    std::vector<SessionEffect> effects;
};

namespace detail {

inline SessionEffect sendEffectFor(RtspStep step) {
    switch (step) {
    case RtspStep::Options:
        return SessionEffect::SendRtspOptions;
    case RtspStep::Describe:
        return SessionEffect::SendRtspDescribe;
    case RtspStep::SetupAudio:
        return SessionEffect::SendRtspSetupAudio;
    case RtspStep::SetupVideo:
        return SessionEffect::SendRtspSetupVideo;
    case RtspStep::SetupControl:
        return SessionEffect::SendRtspSetupControl;
    case RtspStep::Announce:
        return SessionEffect::SendRtspAnnounce;
    case RtspStep::Play:
    default:
        return SessionEffect::SendRtspPlay;
    }
}

inline Reduction fail(SessionState state, SessionFailure reason) {
    state.phase = SessionPhase::Failed;
    state.failure = reason;
    return {state, {SessionEffect::Teardown, SessionEffect::NotifyFailure}};
}

} // namespace detail

// Pure and total: every (phase x event) pair is defined; combinations that do
// not apply return {nullopt, {}} so a stray late completion can never corrupt
// the lifecycle.
inline Reduction reduce(const SessionState& state, const SessionEvent& event) {
    using namespace moon_event;

    // Stop wins from every phase.
    if (std::holds_alternative<StopRequested>(event)) {
        SessionState next; // back to a fresh Idle
        std::vector<SessionEffect> effects;
        if (state.phase == SessionPhase::Streaming ||
            state.phase == SessionPhase::ControlConnecting) {
            effects.push_back(SessionEffect::SendTermination);
        }
        if (state.phase != SessionPhase::Idle) { effects.push_back(SessionEffect::Teardown); }
        return {next, effects};
    }

    switch (state.phase) {
    case SessionPhase::Idle:
    case SessionPhase::Failed: {
        if (std::holds_alternative<StartRequested>(event)) {
            SessionState next;
            next.phase = SessionPhase::CheckingInfo;
            return {next, {SessionEffect::FetchServerInfo}};
        }
        return {std::nullopt, {}};
    }

    case SessionPhase::CheckingInfo: {
        if (const auto* info = std::get_if<ServerInfoOk>(&event)) {
            // A host that came back with a different identity is not the host
            // the stored certificate anchors, so it is named before pairing is
            // judged at all: re-pairing is the only way back either way, and
            // "no longer recognises this device" would be the wrong reason.
            if (info->identityChanged) { return detail::fail(state, SessionFailure::HostReplaced); }
            if (!info->paired) {
                return detail::fail(state, info->remembered ? SessionFailure::TrustLost
                                                            : SessionFailure::NotPaired);
            }
            SessionState next = state;
            next.phase = SessionPhase::Launching;
            next.resuming = info->currentGame != 0 && info->currentGame != -1;
            return {next, {SessionEffect::SendLaunch}};
        }
        if (std::holds_alternative<ServerInfoFailed>(event)) {
            return detail::fail(state, SessionFailure::Unreachable);
        }
        return {std::nullopt, {}};
    }

    case SessionPhase::Launching: {
        if (std::holds_alternative<LaunchOk>(event)) {
            SessionState next = state;
            next.phase = SessionPhase::Rtsp;
            next.rtspStep = RtspStep::Options;
            return {next, {SessionEffect::OpenRtsp}};
        }
        if (const auto* busy = std::get_if<LaunchBusy>(&event)) {
            if (busy->resumable && !state.resuming) {
                SessionState next = state;
                next.resuming = true;
                return {next, {SessionEffect::SendLaunch}};
            }
            return detail::fail(state, SessionFailure::AppAlreadyRunning);
        }
        if (std::holds_alternative<LaunchFailed>(event)) {
            // A /resume that fails is not a refused /launch: the host HAS the
            // session and would not hand it back, which the user fixes by
            // closing the app rather than by trying again.
            return detail::fail(state, state.resuming ? SessionFailure::ResumeFailed
                                                      : SessionFailure::LaunchRejected);
        }
        return {std::nullopt, {}};
    }

    case SessionPhase::Rtsp: {
        if (std::holds_alternative<RtspReady>(event)) {
            if (state.rtspStep != RtspStep::Options) { return {std::nullopt, {}}; }
            return {state, {SessionEffect::SendRtspOptions}};
        }
        if (std::holds_alternative<RtspStepOk>(event)) {
            if (state.rtspStep == RtspStep::Play) {
                SessionState next = state;
                next.phase = SessionPhase::ControlConnecting;
                return {next, {SessionEffect::ConnectControl}};
            }
            SessionState next = state;
            next.rtspStep = static_cast<RtspStep>(static_cast<std::uint8_t>(state.rtspStep) + 1);
            return {next, {detail::sendEffectFor(next.rtspStep)}};
        }
        if (std::holds_alternative<RtspFailed>(event)) {
            return detail::fail(state, SessionFailure::RtspRejected);
        }
        return {std::nullopt, {}};
    }

    case SessionPhase::ControlConnecting: {
        if (std::holds_alternative<ControlConnected>(event)) {
            SessionState next = state;
            next.phase = SessionPhase::Streaming;
            return {next, {SessionEffect::StartStreaming}};
        }
        if (std::holds_alternative<ControlLost>(event)) {
            return detail::fail(state, SessionFailure::ControlLost);
        }
        if (std::holds_alternative<HostTerminated>(event)) {
            return detail::fail(state, SessionFailure::HostEnded);
        }
        return {std::nullopt, {}};
    }

    case SessionPhase::Streaming:
    default: {
        // A link that dies after going live is a DROP, not a setup failure: the
        // host keeps the app and will usually let us resume it, so the two must
        // not be merged.
        if (std::holds_alternative<ControlLost>(event)) {
            return detail::fail(state, SessionFailure::Dropped);
        }
        if (std::holds_alternative<HostTerminated>(event)) {
            return detail::fail(state, SessionFailure::HostEnded);
        }
        return {std::nullopt, {}};
    }
    }
}

} // namespace dish::moonlight
