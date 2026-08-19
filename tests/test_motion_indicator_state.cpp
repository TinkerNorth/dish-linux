// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The gyro chip shows exactly one reason, picked by this precedence:
// Unavailable > UserDisabled > NotForwarded > NoHostSink > BackendBroken >
// Stalled > Streaming/Paused.

#include "core/reducer/MotionIndicatorState.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::motionIndicatorFor;
using dish::reducer::MotionIndicatorInputs;
using dish::reducer::MotionIndicatorState;
using dish::reducer::motionRateUserFacingOn;
using dish::reducer::screenRateUserFacingOn;

namespace {

// Healthy and streaming; each case knocks out one fact to drive one rung.
MotionIndicatorInputs streamingBaseline() {
    MotionIndicatorInputs in;
    in.hasGyro = true;
    in.userEnabled = true;
    in.carriesOnConnection = true;
    in.hostHasSinkForType = true;
    in.backendOk = true;
    in.isStreaming = true;
    in.isPaused = false;
    return in;
}

} // namespace

TEST_CASE("motionIndicator: no gyro is unavailable", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.hasGyro = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::Unavailable);
}

TEST_CASE("motionIndicator: no gyro wins over every other blocking condition", "[motionind]") {
    MotionIndicatorInputs in;
    in.hasGyro = false;
    in.userEnabled = false;
    in.carriesOnConnection = false;
    in.hostHasSinkForType = false;
    in.backendOk = false;
    in.isStreaming = false;
    in.isPaused = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::Unavailable);
}

TEST_CASE("motionIndicator: no gyro unavailable regardless of streaming/paused", "[motionind]") {
    MotionIndicatorInputs a = streamingBaseline();
    a.hasGyro = false;
    a.isStreaming = true;
    a.isPaused = true;
    REQUIRE(motionIndicatorFor(a) == MotionIndicatorState::Unavailable);

    MotionIndicatorInputs b;
    b.hasGyro = false;
    REQUIRE(motionIndicatorFor(b) == MotionIndicatorState::Unavailable);
}

TEST_CASE("motionIndicator: gyro present but user disabled", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.userEnabled = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::UserDisabled);
}

TEST_CASE("motionIndicator: user disabled wins over not-forwarded", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.userEnabled = false;
    in.carriesOnConnection = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::UserDisabled);
}

TEST_CASE("motionIndicator: user disabled wins over no-host-sink and backend", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.userEnabled = false;
    in.hostHasSinkForType = false;
    in.backendOk = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::UserDisabled);
}

TEST_CASE("motionIndicator: user disabled regardless of stall/stream", "[motionind]") {
    MotionIndicatorInputs a = streamingBaseline();
    a.userEnabled = false;
    a.isStreaming = false;
    REQUIRE(motionIndicatorFor(a) == MotionIndicatorState::UserDisabled);

    MotionIndicatorInputs b = streamingBaseline();
    b.userEnabled = false;
    b.isStreaming = true;
    b.isPaused = true;
    REQUIRE(motionIndicatorFor(b) == MotionIndicatorState::UserDisabled);
}

TEST_CASE("motionIndicator: enabled but not forwarded", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.carriesOnConnection = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::NotForwarded);
}

TEST_CASE("motionIndicator: not-forwarded wins over no-host-sink", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.carriesOnConnection = false;
    in.hostHasSinkForType = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::NotForwarded);
}

TEST_CASE("motionIndicator: not-forwarded wins over backend broken", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.carriesOnConnection = false;
    in.backendOk = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::NotForwarded);
}

TEST_CASE("motionIndicator: not-forwarded regardless of stream/stall", "[motionind]") {
    MotionIndicatorInputs a = streamingBaseline();
    a.carriesOnConnection = false;
    a.isStreaming = false;
    REQUIRE(motionIndicatorFor(a) == MotionIndicatorState::NotForwarded);

    MotionIndicatorInputs b = streamingBaseline();
    b.carriesOnConnection = false;
    b.isStreaming = true;
    REQUIRE(motionIndicatorFor(b) == MotionIndicatorState::NotForwarded);
}

TEST_CASE("motionIndicator: forwarded but no host sink for type", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.hostHasSinkForType = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::NoHostSink);
}

TEST_CASE("motionIndicator: no-host-sink wins over backend broken", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.hostHasSinkForType = false;
    in.backendOk = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::NoHostSink);
}

TEST_CASE("motionIndicator: no-host-sink regardless of stream/stall", "[motionind]") {
    MotionIndicatorInputs a = streamingBaseline();
    a.hostHasSinkForType = false;
    a.isStreaming = false;
    REQUIRE(motionIndicatorFor(a) == MotionIndicatorState::NoHostSink);

    MotionIndicatorInputs b = streamingBaseline();
    b.hostHasSinkForType = false;
    b.isStreaming = true;
    b.isPaused = true;
    REQUIRE(motionIndicatorFor(b) == MotionIndicatorState::NoHostSink);
}

TEST_CASE("motionIndicator: host sink present but backend broken", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.backendOk = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::BackendBroken);
}

TEST_CASE("motionIndicator: backend broken wins over stall and stream", "[motionind]") {
    MotionIndicatorInputs a = streamingBaseline();
    a.backendOk = false;
    a.isStreaming = false;
    REQUIRE(motionIndicatorFor(a) == MotionIndicatorState::BackendBroken);

    MotionIndicatorInputs b = streamingBaseline();
    b.backendOk = false;
    b.isStreaming = true;
    REQUIRE(motionIndicatorFor(b) == MotionIndicatorState::BackendBroken);
}

TEST_CASE("motionIndicator: backend broken with paused still broken", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.backendOk = false;
    in.isStreaming = false;
    in.isPaused = true;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::BackendBroken);
}

TEST_CASE("motionIndicator: fully healthy and streaming", "[motionind]") {
    REQUIRE(motionIndicatorFor(streamingBaseline()) == MotionIndicatorState::Streaming);
}

TEST_CASE("motionIndicator: streaming wins over a stale paused flag", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.isStreaming = true;
    in.isPaused = true;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::Streaming);
}

TEST_CASE("motionIndicator: healthy not-streaming but paused is paused", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.isStreaming = false;
    in.isPaused = true;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::Paused);
}

TEST_CASE("motionIndicator: healthy but no samples and not paused is stalled", "[motionind]") {
    MotionIndicatorInputs in = streamingBaseline();
    in.isStreaming = false;
    in.isPaused = false;
    REQUIRE(motionIndicatorFor(in) == MotionIndicatorState::Stalled);
}

TEST_CASE("motionIndicator: stalled requires all upstream conditions healthy", "[motionind]") {
    MotionIndicatorInputs base = streamingBaseline();
    base.isStreaming = false;
    REQUIRE(motionIndicatorFor(base) == MotionIndicatorState::Stalled);

    MotionIndicatorInputs noSink = base;
    noSink.hostHasSinkForType = false;
    REQUIRE(motionIndicatorFor(noSink) == MotionIndicatorState::NoHostSink);
}

TEST_CASE("motionIndicator: full precedence sweep across rungs", "[motionind]") {
    // Each case satisfies one rung's trigger AND every lower trigger, so the
    // higher rung winning is what pins the strict ordering.
    struct Case {
        MotionIndicatorInputs in;
        MotionIndicatorState expect;
    };

    MotionIndicatorInputs allBad;
    allBad.hasGyro = true;
    allBad.userEnabled = false;
    allBad.carriesOnConnection = false;
    allBad.hostHasSinkForType = false;
    allBad.backendOk = false;
    allBad.isStreaming = false;
    allBad.isPaused = false;
    REQUIRE(motionIndicatorFor(allBad) == MotionIndicatorState::UserDisabled);

    MotionIndicatorInputs c2 = allBad;
    c2.userEnabled = true;
    REQUIRE(motionIndicatorFor(c2) == MotionIndicatorState::NotForwarded);

    MotionIndicatorInputs c3 = c2;
    c3.carriesOnConnection = true;
    REQUIRE(motionIndicatorFor(c3) == MotionIndicatorState::NoHostSink);

    MotionIndicatorInputs c4 = c3;
    c4.hostHasSinkForType = true;
    REQUIRE(motionIndicatorFor(c4) == MotionIndicatorState::BackendBroken);

    MotionIndicatorInputs c5 = c4;
    c5.backendOk = true;
    REQUIRE(motionIndicatorFor(c5) == MotionIndicatorState::Stalled);

    MotionIndicatorInputs c6 = c5;
    c6.isPaused = true;
    REQUIRE(motionIndicatorFor(c6) == MotionIndicatorState::Paused);

    MotionIndicatorInputs c7 = c6;
    c7.isStreaming = true;
    REQUIRE(motionIndicatorFor(c7) == MotionIndicatorState::Streaming);
}

TEST_CASE("motionRateUserFacingOn: hidden without a motion capability", "[motionind]") {
    REQUIRE_FALSE(motionRateUserFacingOn(false, MotionIndicatorState::Streaming));
    REQUIRE_FALSE(motionRateUserFacingOn(false, MotionIndicatorState::UserDisabled));
}

TEST_CASE("motionRateUserFacingOn: hidden when unavailable even if capable", "[motionind]") {
    REQUIRE_FALSE(motionRateUserFacingOn(true, MotionIndicatorState::Unavailable));
}

TEST_CASE("motionRateUserFacingOn: shown when capable and disabled", "[motionind]") {
    // A disabled-but-capable slot still shows the (0 Hz) meter for context.
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::UserDisabled));
}

TEST_CASE("motionRateUserFacingOn: shown when capable and streaming", "[motionind]") {
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::Streaming));
}

TEST_CASE("motionRateUserFacingOn: shown when capable across other states", "[motionind]") {
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::NotForwarded));
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::NoHostSink));
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::BackendBroken));
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::Stalled));
    REQUIRE(motionRateUserFacingOn(true, MotionIndicatorState::Paused));
}

TEST_CASE("screenRateUserFacingOn: hidden when not connected", "[motionind]") {
    REQUIRE_FALSE(screenRateUserFacingOn(false, true));
}

TEST_CASE("screenRateUserFacingOn: hidden when touchpad forwarding off", "[motionind]") {
    REQUIRE_FALSE(screenRateUserFacingOn(true, false));
}

TEST_CASE("screenRateUserFacingOn: shown only when connected and forwarding on", "[motionind]") {
    REQUIRE(screenRateUserFacingOn(true, true));
}

TEST_CASE("screenRateUserFacingOn: hidden when neither connected nor forwarding", "[motionind]") {
    REQUIRE_FALSE(screenRateUserFacingOn(false, false));
}
