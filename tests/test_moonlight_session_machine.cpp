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
        {at(SessionPhase::Streaming), ControlLost{}, SessionFailure::ControlLost},
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
