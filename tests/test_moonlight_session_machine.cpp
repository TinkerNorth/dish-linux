// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The full decision space of the Moonlight session reducer: the happy path,
// every failure edge, stop-from-anywhere, and the no-op guarantee for stale
// completions.

#include "core/moonlight/MoonlightSessionMachine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace dish::moonlight;
using namespace dish::moonlight::moon_event;

namespace {

bool hasEffect(const Reduction& r, SessionEffect e) {
    return std::find(r.effects.begin(), r.effects.end(), e) != r.effects.end();
}

SessionState at(SessionPhase phase, RtspStep step = RtspStep::Options) {
    SessionState s;
    s.phase = phase;
    s.rtspStep = step;
    return s;
}

} // namespace

TEST_CASE("happy path from Idle to Streaming", "[moonlight][machine]") {
    SessionState s;

    auto r = reduce(s, StartRequested{});
    REQUIRE(r.next.has_value());
    s = *r.next;
    CHECK(s.phase == SessionPhase::CheckingInfo);
    CHECK(hasEffect(r, SessionEffect::FetchServerInfo));

    r = reduce(s, ServerInfoOk{true, 0});
    REQUIRE(r.next.has_value());
    s = *r.next;
    CHECK(s.phase == SessionPhase::Launching);
    CHECK_FALSE(s.resuming);
    CHECK(hasEffect(r, SessionEffect::SendLaunch));

    r = reduce(s, LaunchOk{});
    REQUIRE(r.next.has_value());
    s = *r.next;
    CHECK(s.phase == SessionPhase::Rtsp);
    CHECK(s.rtspStep == RtspStep::Options);
    CHECK(hasEffect(r, SessionEffect::OpenRtsp));

    r = reduce(s, RtspReady{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Rtsp);
    CHECK(hasEffect(r, SessionEffect::SendRtspOptions));

    // Options → Describe → SetupAudio → SetupVideo → SetupControl → Announce → Play.
    const SessionEffect order[] = {
        SessionEffect::SendRtspDescribe,   SessionEffect::SendRtspSetupAudio,
        SessionEffect::SendRtspSetupVideo, SessionEffect::SendRtspSetupControl,
        SessionEffect::SendRtspAnnounce,   SessionEffect::SendRtspPlay,
    };
    for (const SessionEffect expected : order) {
        r = reduce(s, RtspStepOk{});
        REQUIRE(r.next.has_value());
        s = *r.next;
        CHECK(s.phase == SessionPhase::Rtsp);
        CHECK(hasEffect(r, expected));
    }
    CHECK(s.rtspStep == RtspStep::Play);

    r = reduce(s, RtspStepOk{}); // PLAY answered
    REQUIRE(r.next.has_value());
    s = *r.next;
    CHECK(s.phase == SessionPhase::ControlConnecting);
    CHECK(hasEffect(r, SessionEffect::ConnectControl));

    r = reduce(s, ControlConnected{});
    REQUIRE(r.next.has_value());
    s = *r.next;
    CHECK(s.phase == SessionPhase::Streaming);
    CHECK(hasEffect(r, SessionEffect::StartStreaming));
}

TEST_CASE("a running app resumes instead of launching", "[moonlight][machine]") {
    const auto r = reduce(at(SessionPhase::CheckingInfo), ServerInfoOk{true, 123456});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Launching);
    CHECK(r.next->resuming);
}

TEST_CASE("currentgame of -1 means nothing running", "[moonlight][machine]") {
    const auto r = reduce(at(SessionPhase::CheckingInfo), ServerInfoOk{true, -1});
    REQUIRE(r.next.has_value());
    CHECK_FALSE(r.next->resuming);
}

TEST_CASE("failure edges land in Failed with teardown + notify", "[moonlight][machine]") {
    struct Case {
        SessionState from;
        SessionEvent event;
        SessionFailure expected;
    };
    const Case cases[] = {
        {at(SessionPhase::CheckingInfo), ServerInfoFailed{}, SessionFailure::Unreachable},
        {at(SessionPhase::CheckingInfo), ServerInfoOk{false, 0}, SessionFailure::NotPaired},
        {at(SessionPhase::Launching), LaunchFailed{}, SessionFailure::LaunchRejected},
        {at(SessionPhase::Rtsp, RtspStep::SetupControl), RtspFailed{},
         SessionFailure::RtspRejected},
        {at(SessionPhase::ControlConnecting), ControlLost{}, SessionFailure::ControlLost},
        {at(SessionPhase::ControlConnecting), HostTerminated{}, SessionFailure::HostEnded},
        {at(SessionPhase::Streaming), ControlLost{}, SessionFailure::Dropped},
        {at(SessionPhase::Streaming), HostTerminated{}, SessionFailure::HostEnded},
    };
    for (const auto& c : cases) {
        const auto r = reduce(c.from, c.event);
        REQUIRE(r.next.has_value());
        CHECK(r.next->phase == SessionPhase::Failed);
        REQUIRE(r.next->failure.has_value());
        CHECK(*r.next->failure == c.expected);
        CHECK(hasEffect(r, SessionEffect::Teardown));
        CHECK(hasEffect(r, SessionEffect::NotifyFailure));
    }
}

TEST_CASE("a resumable in-body refusal promotes the launch to a resume", "[moonlight][machine]") {
    // /launch answered HTTP 200 with status_code="400", "An app is already
    // running on this host" and <resume>1</resume>: the host will hand that
    // session over, so ask it to, rather than giving up.
    const auto r = reduce(at(SessionPhase::Launching), LaunchBusy{true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Launching);
    CHECK(r.next->resuming);
    CHECK_FALSE(r.next->failure.has_value());
    CHECK(hasEffect(r, SessionEffect::SendLaunch));
    CHECK_FALSE(hasEffect(r, SessionEffect::Teardown));
}

TEST_CASE("a refusal with no resume offer ends the attempt", "[moonlight][machine]") {
    const auto r = reduce(at(SessionPhase::Launching), LaunchBusy{false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Failed);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::AppAlreadyRunning);
    CHECK(hasEffect(r, SessionEffect::Teardown));
    CHECK(hasEffect(r, SessionEffect::NotifyFailure));
}

TEST_CASE("a resume that is refused again does not loop", "[moonlight][machine]") {
    SessionState resuming = at(SessionPhase::Launching);
    resuming.resuming = true;
    const auto r = reduce(resuming, LaunchBusy{true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Failed);
    REQUIRE(r.next->failure.has_value());
    CHECK(*r.next->failure == SessionFailure::AppAlreadyRunning);
    CHECK_FALSE(hasEffect(r, SessionEffect::SendLaunch));
}

TEST_CASE("a busy reply outside Launching is ignored", "[moonlight][machine]") {
    const SessionPhase elsewhere[] = {SessionPhase::Idle, SessionPhase::CheckingInfo,
                                      SessionPhase::Rtsp, SessionPhase::ControlConnecting,
                                      SessionPhase::Streaming};
    for (const SessionPhase phase : elsewhere) {
        CHECK_FALSE(reduce(at(phase), LaunchBusy{true}).next.has_value());
        CHECK_FALSE(reduce(at(phase), LaunchBusy{false}).next.has_value());
    }
}

TEST_CASE("a serverinfo that already names a running app resumes straight away",
          "[moonlight][machine]") {
    const auto r = reduce(at(SessionPhase::CheckingInfo), ServerInfoOk{true, 881448767});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::Launching);
    CHECK(r.next->resuming);
    CHECK(hasEffect(r, SessionEffect::SendLaunch));
    // Sunshine spells "nothing running" as either 0 or -1.
    for (const int idle : {0, -1}) {
        const auto fresh = reduce(at(SessionPhase::CheckingInfo), ServerInfoOk{true, idle});
        REQUIRE(fresh.next.has_value());
        CHECK_FALSE(fresh.next->resuming);
    }
}

TEST_CASE("stop wins from every phase", "[moonlight][machine]") {
    const SessionPhase all[] = {
        SessionPhase::Idle,  SessionPhase::CheckingInfo,      SessionPhase::Launching,
        SessionPhase::Rtsp,  SessionPhase::ControlConnecting, SessionPhase::Streaming,
        SessionPhase::Failed};
    for (const SessionPhase phase : all) {
        const auto r = reduce(at(phase), StopRequested{});
        REQUIRE(r.next.has_value());
        CHECK(r.next->phase == SessionPhase::Idle);
        CHECK_FALSE(r.next->failure.has_value());
        // TERMINATION only where the encrypted link exists.
        const bool linkUp =
            phase == SessionPhase::Streaming || phase == SessionPhase::ControlConnecting;
        CHECK(hasEffect(r, SessionEffect::SendTermination) == linkUp);
        CHECK(hasEffect(r, SessionEffect::Teardown) == (phase != SessionPhase::Idle));
    }
}

TEST_CASE("Failed is restartable", "[moonlight][machine]") {
    SessionState failed = at(SessionPhase::Failed);
    failed.failure = SessionFailure::ControlLost;
    const auto r = reduce(failed, StartRequested{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == SessionPhase::CheckingInfo);
    CHECK_FALSE(r.next->failure.has_value());
    CHECK(hasEffect(r, SessionEffect::FetchServerInfo));
}

TEST_CASE("stale completions are no-ops in every non-matching phase", "[moonlight][machine]") {
    const SessionEvent completions[] = {
        ServerInfoOk{true, 0}, ServerInfoFailed{}, LaunchOk{},   LaunchFailed{},
        RtspReady{},           RtspStepOk{},       RtspFailed{}, ControlConnected{},
    };
    // Idle must shrug off every completion event.
    for (const auto& event : completions) {
        const auto r = reduce(at(SessionPhase::Idle), event);
        CHECK_FALSE(r.next.has_value());
        CHECK(r.effects.empty());
    }
    // A late launch reply after RTSP started is ignored.
    CHECK_FALSE(reduce(at(SessionPhase::Rtsp), LaunchOk{}).next.has_value());
    // A late RTSP reply after the control link opened is ignored.
    CHECK_FALSE(reduce(at(SessionPhase::ControlConnecting), RtspStepOk{}).next.has_value());
    // RtspReady only applies at the Options step.
    CHECK_FALSE(reduce(at(SessionPhase::Rtsp, RtspStep::Announce), RtspReady{}).next.has_value());
    // Streaming ignores connect-phase noise.
    CHECK_FALSE(reduce(at(SessionPhase::Streaming), ControlConnected{}).next.has_value());
    // ControlLost before any control link exists is ignored.
    CHECK_FALSE(reduce(at(SessionPhase::Launching), ControlLost{}).next.has_value());
    // A second StartRequested mid-flight is ignored.
    CHECK_FALSE(reduce(at(SessionPhase::CheckingInfo), StartRequested{}).next.has_value());
    CHECK_FALSE(reduce(at(SessionPhase::Streaming), StartRequested{}).next.has_value());
}

TEST_CASE("RtspReady at Options re-sends OPTIONS without advancing", "[moonlight][machine]") {
    const auto r = reduce(at(SessionPhase::Rtsp, RtspStep::Options), RtspReady{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->rtspStep == RtspStep::Options);
    CHECK(hasEffect(r, SessionEffect::SendRtspOptions));
}

TEST_CASE("a host that answers unpaired names what is remembered", "[moonlight][machine]") {
    // NOT PAIRED and TRUST LOST are the same wire fact and two different
    // sentences: one asks for a first pairing, the other says the host deleted
    // one we still hold a certificate for.
    ServerInfoOk fresh;
    fresh.paired = false;
    fresh.remembered = false;
    auto r = reduce(at(SessionPhase::CheckingInfo), fresh);
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::NotPaired);

    ServerInfoOk forgotten;
    forgotten.paired = false;
    forgotten.remembered = true;
    r = reduce(at(SessionPhase::CheckingInfo), forgotten);
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::TrustLost);
    CHECK(hasEffect(r, SessionEffect::NotifyFailure));
}

TEST_CASE("a host with a new identity is named before pairing is judged", "[moonlight][machine]") {
    // The stored certificate anchors nothing on a machine that was reset, so
    // "no longer recognises this device" would be the wrong reason.
    ServerInfoOk replaced;
    replaced.paired = true;
    replaced.remembered = true;
    replaced.identityChanged = true;
    auto r = reduce(at(SessionPhase::CheckingInfo), replaced);
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::HostReplaced);

    // Even when the host also reports us unpaired.
    replaced.paired = false;
    r = reduce(at(SessionPhase::CheckingInfo), replaced);
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::HostReplaced);
}

TEST_CASE("a failed resume is not a refused launch", "[moonlight][machine]") {
    // The host HAS the session and would not hand it back, which the user fixes
    // by closing the app rather than by trying the same thing again.
    SessionState resuming = at(SessionPhase::Launching);
    resuming.resuming = true;
    auto r = reduce(resuming, LaunchFailed{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::ResumeFailed);

    // A first launch that fails is still a plain refusal.
    r = reduce(at(SessionPhase::Launching), LaunchFailed{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::LaunchRejected);
}

TEST_CASE("a link that dies after going live is a drop, not a setup failure",
          "[moonlight][machine]") {
    // The host keeps the app and will usually let us resume it. Merging the two
    // would offer a Reconnect that cannot work, or a retry that closes a game.
    auto r = reduce(at(SessionPhase::ControlConnecting), ControlLost{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::ControlLost);

    r = reduce(at(SessionPhase::Streaming), ControlLost{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->failure == SessionFailure::Dropped);

    // A host that ends the session says so itself, from either phase.
    for (const SessionPhase phase : {SessionPhase::ControlConnecting, SessionPhase::Streaming}) {
        const auto ended = reduce(at(phase), HostTerminated{});
        REQUIRE(ended.next.has_value());
        CHECK(ended.next->failure == SessionFailure::HostEnded);
    }
}

TEST_CASE("a session starts only from a resting phase", "[moonlight][machine]") {
    // The reference count: a second binding on a host that is already checking,
    // launching or live joins that session and must not launch a second.
    CHECK(sessionNeedsStart(SessionPhase::Idle));
    CHECK(sessionNeedsStart(SessionPhase::Failed));
    CHECK_FALSE(sessionNeedsStart(SessionPhase::CheckingInfo));
    CHECK_FALSE(sessionNeedsStart(SessionPhase::Launching));
    CHECK_FALSE(sessionNeedsStart(SessionPhase::Rtsp));
    CHECK_FALSE(sessionNeedsStart(SessionPhase::ControlConnecting));
    CHECK_FALSE(sessionNeedsStart(SessionPhase::Streaming));
}
