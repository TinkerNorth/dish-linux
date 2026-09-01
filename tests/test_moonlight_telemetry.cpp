// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The satellite-to-Moonlight unit translation, and the host's motion
// subscription gate.
//
// The units matter more than they look: both wires carry "a gyro sample", but
// one is fixed-point at a declared full scale and the other is a float in a
// physical unit, and accel additionally changes unit (g to m/s^2). A conversion
// that drops the gravity factor under-reports by 9.8x, which a host reads as a
// pad lying perfectly still however hard it is shaken.

#include "core/moonlight/MoonlightTelemetry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using dish::moonlight::accelMs2;
using dish::moonlight::gyroDegS;
using dish::moonlight::kStandardGravity;
using dish::moonlight::MoonlightMotionGate;
using dish::moonlight::touchNorm;

namespace moon = dish::moonproto;

TEST_CASE("gyro converts from the satellite's full scale to deg/s", "[moonlight][telemetry]") {
    // Full scale is +/-2000 deg/s over int16, so the extremes are the check that
    // matters; a wrong divisor shows up there first.
    CHECK(gyroDegS(0) == Approx(0.0F));
    CHECK(gyroDegS(32767) == Approx(2000.0F));
    CHECK(gyroDegS(-32767) == Approx(-2000.0F));
    CHECK(gyroDegS(16384) == Approx(1000.03F).margin(0.05));
}

TEST_CASE("accel converts to metres per second squared, not g", "[moonlight][telemetry]") {
    // +/-4 g full scale, and the wire wants m/s^2. The gravity factor is the
    // whole point of this case.
    CHECK(accelMs2(0) == Approx(0.0F));
    CHECK(accelMs2(32767) == Approx(4.0F * kStandardGravity));
    CHECK(accelMs2(-32767) == Approx(-4.0F * kStandardGravity));
    // One g of gravity, as a pad resting flat reports it.
    CHECK(accelMs2(8192) == Approx(kStandardGravity).margin(0.01));
    // Explicitly NOT the g value: this is the regression that reads as a dead
    // sensor rather than as an error.
    CHECK(accelMs2(32767) != Approx(4.0F));
}

TEST_CASE("touch coordinates normalise to a closed 0..1", "[moonlight][telemetry]") {
    // Both ends have to be exact: a finger at the pad's edge landing at 0.5
    // would put every gesture in the middle of the host's touchpad.
    CHECK(touchNorm(-32768) == Approx(0.0F));
    CHECK(touchNorm(32767) == Approx(1.0F));
    CHECK(touchNorm(0) == Approx(0.5F).margin(0.0001));
}

TEST_CASE("motion stays off until the host asks", "[moonlight][motiongate]") {
    // The default is what keeps an unasked-for IMU stream off the wire. A gate
    // that defaulted open would stream to every host that never wanted it.
    MoonlightMotionGate gate;
    CHECK_FALSE(gate.wanted(0, moon::kMotionGyroscope));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 0));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionAcceleration, 1000000));
}

TEST_CASE("a subscription opens exactly one pad and one motion type", "[moonlight][motiongate]") {
    // Sunshine subscribes per type: a game that opened the gyro must not start
    // receiving accel, and pad 0's subscription is not pad 1's.
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    CHECK(gate.wanted(0, moon::kMotionGyroscope));
    CHECK_FALSE(gate.wanted(0, moon::kMotionAcceleration));
    CHECK_FALSE(gate.wanted(1, moon::kMotionGyroscope));
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 0));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionAcceleration, 0));
    CHECK_FALSE(gate.shouldSend(1, moon::kMotionGyroscope, 0));
}

TEST_CASE("samples faster than the requested rate are dropped", "[moonlight][motiongate]") {
    // 100 Hz is a 10 ms floor. A pad polling at 250 Hz hands over a sample every
    // 4 ms, and the extra ones are dropped rather than queued: they are stale by
    // the time the host would read them.
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 0));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 4000));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 9999));
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 10000));
    // ...and the window restarts from the accepted sample, not from the clock.
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 15000));
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 20000));
}

TEST_CASE("each type keeps its own cadence", "[moonlight][motiongate]") {
    // A shared timestamp would let a gyro sample consume the accel budget, so a
    // host asking for both would receive half of each.
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    gate.onMotionRequest(0, 100, moon::kMotionAcceleration);
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 0));
    CHECK(gate.shouldSend(0, moon::kMotionAcceleration, 0));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 1000));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionAcceleration, 1000));
}

TEST_CASE("different rates give different floors", "[moonlight][motiongate]") {
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 200, moon::kMotionGyroscope); // 5 ms
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 0));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 4999));
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 5000));
}

TEST_CASE("rate 0 is how a host says stop", "[moonlight][motiongate]") {
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    REQUIRE(gate.shouldSend(0, moon::kMotionGyroscope, 0));
    gate.onMotionRequest(0, 0, moon::kMotionGyroscope);
    CHECK_FALSE(gate.wanted(0, moon::kMotionGyroscope));
    CHECK_FALSE(gate.shouldSend(0, moon::kMotionGyroscope, 1000000));
}

TEST_CASE("an unsubscribe clears the cadence too", "[moonlight][motiongate]") {
    // Otherwise a re-subscribe would inherit the old timestamp and drop its
    // first sample for no reason.
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    REQUIRE(gate.shouldSend(0, moon::kMotionGyroscope, 50000));
    gate.onMotionRequest(0, 0, moon::kMotionGyroscope);
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    CHECK(gate.shouldSend(0, moon::kMotionGyroscope, 50001));
}

TEST_CASE("a negative rate is treated as a stop", "[moonlight][motiongate]") {
    // Nothing on the wire should produce one, but a negative interval would
    // divide into a nonsense floor rather than failing loudly.
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    gate.onMotionRequest(0, -5, moon::kMotionGyroscope);
    CHECK_FALSE(gate.wanted(0, moon::kMotionGyroscope));
}

TEST_CASE("clearing one pad leaves the others subscribed", "[moonlight][motiongate]") {
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    gate.onMotionRequest(0, 100, moon::kMotionAcceleration);
    gate.onMotionRequest(1, 100, moon::kMotionGyroscope);
    gate.clear(0);
    CHECK_FALSE(gate.wanted(0, moon::kMotionGyroscope));
    CHECK_FALSE(gate.wanted(0, moon::kMotionAcceleration));
    CHECK(gate.wanted(1, moon::kMotionGyroscope));
}

TEST_CASE("a returning pad waits to be asked again", "[moonlight][motiongate]") {
    // clear() on unbind is what stops a re-bound pad resuming a stream the host
    // has forgotten it ever requested.
    MoonlightMotionGate gate;
    gate.onMotionRequest(2, 100, moon::kMotionGyroscope);
    gate.clear(2);
    CHECK_FALSE(gate.shouldSend(2, moon::kMotionGyroscope, 0));
    gate.onMotionRequest(2, 100, moon::kMotionGyroscope);
    CHECK(gate.shouldSend(2, moon::kMotionGyroscope, 0));
}

TEST_CASE("clearAll drops every subscription", "[moonlight][motiongate]") {
    MoonlightMotionGate gate;
    gate.onMotionRequest(0, 100, moon::kMotionGyroscope);
    gate.onMotionRequest(1, 100, moon::kMotionAcceleration);
    gate.clearAll();
    CHECK_FALSE(gate.wanted(0, moon::kMotionGyroscope));
    CHECK_FALSE(gate.wanted(1, moon::kMotionAcceleration));
}

TEST_CASE("satellite battery status maps onto the Moonlight state", "[moonlight][telemetry]") {
    // The one battery publisher feeds both transports, so this is the whole of
    // the translation between them.
    using dish::moonlight::batteryStateFromSatelliteStatus;
    CHECK(batteryStateFromSatelliteStatus(0) == moon::kBatteryStateUnknown);
    CHECK(batteryStateFromSatelliteStatus(1) == moon::kBatteryDischarging);
    CHECK(batteryStateFromSatelliteStatus(2) == moon::kBatteryCharging);
    CHECK(batteryStateFromSatelliteStatus(3) == moon::kBatteryFull);
    // Wired reads as charging, the closest state the wire has. NOT_PRESENT
    // would tell the host the pad has no battery, which is a different claim.
    CHECK(batteryStateFromSatelliteStatus(4) == moon::kBatteryCharging);
    // A status this build does not know is unknown, never a guess.
    CHECK(batteryStateFromSatelliteStatus(99) == moon::kBatteryStateUnknown);
}

TEST_CASE("an out-of-range battery level reports unknown, not a clamp", "[moonlight][telemetry]") {
    // Clamping would show the user a confident 100% for a pad that never
    // answered, which is worse than showing nothing.
    using dish::moonlight::batteryPercentage;
    CHECK(batteryPercentage(0) == 0);
    CHECK(batteryPercentage(55) == 55);
    CHECK(batteryPercentage(100) == 100);
    CHECK(batteryPercentage(101) == moon::kBatteryPercentageUnknown);
    CHECK(batteryPercentage(0xFF) == moon::kBatteryPercentageUnknown);
}
