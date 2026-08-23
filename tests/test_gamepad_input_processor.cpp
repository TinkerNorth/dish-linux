// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Input/GamepadInputProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

using dish::input::applyDeadzones;
using dish::input::GamepadInputProcessor;
using dish::input::isActuation;
using dish::input::kActuationStickEpsilon;
using dish::input::kActuationTriggerEpsilon;
using dish::input::scaleAxis;
using dish::input::scaleTrigger;

TEST_CASE("scaleAxis clamps inputs to [-1.0, 1.0]", "[input]") {
    REQUIRE(scaleAxis(-2.0F, 32767.0F) == INT16_MIN + 1); // 32767 magnitude clamps to -32767
    REQUIRE(scaleAxis(2.0F, 32767.0F) == 32767);
    REQUIRE(scaleAxis(0.0F, 32767.0F) == 0);
    REQUIRE(scaleAxis(0.5F, 32767.0F) == 16383);
}

TEST_CASE("scaleTrigger clamps and rounds to [0, 255]", "[input]") {
    REQUIRE(scaleTrigger(-1.0F) == 0);
    REQUIRE(scaleTrigger(0.0F) == 0);
    REQUIRE(scaleTrigger(0.5F) == 128);
    REQUIRE(scaleTrigger(1.0F) == 255);
    REQUIRE(scaleTrigger(2.0F) == 255);
}

TEST_CASE("publish forwards every state to the report sender", "[input]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint16_t lastButtons = 0;
    p.setReportSender([&](const std::string& id, std::uint16_t b, std::uint8_t, std::uint8_t,
                          std::int16_t, std::int16_t, std::int16_t, std::int16_t) {
        ++calls;
        lastButtons = b;
        REQUIRE(id == "pad-1");
    });

    GamepadInputProcessor::DeviceState s;
    s.wButtons = GamepadInputProcessor::Buttons::kA;
    p.publish("pad-1", s);
    s.wButtons |= GamepadInputProcessor::Buttons::kB;
    p.publish("pad-1", s);

    REQUIRE(calls == 2);
    REQUIRE(lastButtons ==
            (GamepadInputProcessor::Buttons::kA | GamepadInputProcessor::Buttons::kB));
}

TEST_CASE("zeroAndSendAll emits a neutral report for every known device", "[input]") {
    GamepadInputProcessor p;
    int zeros = 0;
    p.setReportSender([&](const std::string&, std::uint16_t b, std::uint8_t lt, std::uint8_t rt,
                          std::int16_t lx, std::int16_t ly, std::int16_t rx, std::int16_t ry) {
        if (b == 0 && lt == 0 && rt == 0 && lx == 0 && ly == 0 && rx == 0 && ry == 0) { ++zeros; }
    });

    GamepadInputProcessor::DeviceState a;
    a.wButtons = GamepadInputProcessor::Buttons::kStart;
    a.lt = 100;
    p.publish("pad-a", a);
    GamepadInputProcessor::DeviceState b;
    b.lx = 12345;
    p.publish("pad-b", b);

    p.zeroAndSendAll();
    REQUIRE(zeros == 2);
}

TEST_CASE("drainTelemetry resets per-second counters and keeps lifetime total", "[input]") {
    GamepadInputProcessor p;
    p.setReportSender([](const std::string&, std::uint16_t, std::uint8_t, std::uint8_t,
                         std::int16_t, std::int16_t, std::int16_t, std::int16_t) {});

    GamepadInputProcessor::DeviceState s;
    p.publish("pad", s);
    p.publish("pad", s);
    p.publish("pad", s);

    auto snap = p.drainTelemetry();
    REQUIRE(snap.events == 3);
    REQUIRE(snap.sends == 3);
    REQUIRE(snap.totalSent == 3);

    auto snap2 = p.drainTelemetry();
    REQUIRE(snap2.events == 0);
    REQUIRE(snap2.sends == 0);
    REQUIRE(snap2.totalSent == 3);
}

TEST_CASE("applyDeadzones zeroes sticks at or below threshold", "[input]") {
    GamepadInputProcessor::Deadzones dz{3277, 13};
    GamepadInputProcessor::DeviceState s;
    s.lx = 1500;
    s.ly = -2000;
    s.rx = 3277;
    s.ry = -3277;
    const auto out = applyDeadzones(s, dz);
    REQUIRE(out.lx == 0);
    REQUIRE(out.ly == 0);
    REQUIRE(out.rx == 0);
    REQUIRE(out.ry == 0);
}

TEST_CASE("applyDeadzones passes sticks above threshold", "[input]") {
    GamepadInputProcessor::Deadzones dz{3277, 13};
    GamepadInputProcessor::DeviceState s;
    s.lx = 3278;
    s.ly = -3278;
    s.rx = 32767;
    s.ry = -32767;
    const auto out = applyDeadzones(s, dz);
    REQUIRE(out.lx == 3278);
    REQUIRE(out.ly == -3278);
    REQUIRE(out.rx == 32767);
    REQUIRE(out.ry == -32767);
}

TEST_CASE("applyDeadzones zeroes triggers at or below threshold", "[input]") {
    GamepadInputProcessor::Deadzones dz{0, 13};
    GamepadInputProcessor::DeviceState s;
    s.lt = 5;
    s.rt = 13;
    const auto out = applyDeadzones(s, dz);
    REQUIRE(out.lt == 0);
    REQUIRE(out.rt == 0);
}

TEST_CASE("applyDeadzones passes triggers above threshold", "[input]") {
    GamepadInputProcessor::Deadzones dz{0, 13};
    GamepadInputProcessor::DeviceState s;
    s.lt = 14;
    s.rt = 255;
    const auto out = applyDeadzones(s, dz);
    REQUIRE(out.lt == 14);
    REQUIRE(out.rt == 255);
}

TEST_CASE("applyDeadzones never touches buttons", "[input]") {
    GamepadInputProcessor::Deadzones dz{32767, 255};
    GamepadInputProcessor::DeviceState s;
    s.wButtons = 0xABCD;
    const auto out = applyDeadzones(s, dz);
    REQUIRE(out.wButtons == 0xABCD);
}

TEST_CASE("publish uses per-device deadzones", "[input]") {
    GamepadInputProcessor p;
    std::int16_t lastLx = -1;
    std::int16_t lastLy = -1;
    std::uint8_t lastLt = 0xFF;
    std::uint8_t lastRt = 0xFF;
    p.setReportSender([&](const std::string&, std::uint16_t, std::uint8_t lt, std::uint8_t rt,
                          std::int16_t lx, std::int16_t ly, std::int16_t, std::int16_t) {
        lastLx = lx;
        lastLy = ly;
        lastLt = lt;
        lastRt = rt;
    });
    p.setDeadzones("pad-1", {5000, 20});
    GamepadInputProcessor::DeviceState s;
    s.lx = 4999;
    s.ly = 5001;
    s.lt = 18;
    s.rt = 21;
    p.publish("pad-1", s);
    REQUIRE(lastLx == 0);
    REQUIRE(lastLy == 5001);
    REQUIRE(lastLt == 0);
    REQUIRE(lastRt == 21);
}

TEST_CASE("publish applies different deadzones per device", "[input]") {
    GamepadInputProcessor p;
    std::unordered_map<std::string, std::int16_t> byId;
    p.setReportSender([&](const std::string& id, std::uint16_t, std::uint8_t, std::uint8_t,
                          std::int16_t lx, std::int16_t, std::int16_t,
                          std::int16_t) { byId[id] = lx; });
    p.setDeadzones("lax", {0, 0});
    p.setDeadzones("strict", {10000, 0});
    GamepadInputProcessor::DeviceState s;
    s.lx = 500;
    p.publish("lax", s);
    p.publish("strict", s);
    REQUIRE(byId["lax"] == 500);
    REQUIRE(byId["strict"] == 0);
}

TEST_CASE("remove clears deadzones too", "[input]") {
    GamepadInputProcessor p;
    std::int16_t lastLx = -1;
    p.setReportSender([&](const std::string&, std::uint16_t, std::uint8_t, std::uint8_t,
                          std::int16_t lx, std::int16_t, std::int16_t,
                          std::int16_t) { lastLx = lx; });
    p.setDeadzones("pad", {5000, 0});
    p.remove("pad");
    GamepadInputProcessor::DeviceState s;
    s.lx = 100;
    p.publish("pad", s);
    REQUIRE(lastLx == 100);
}

TEST_CASE("publishMotionAt forwards the first sample with delta 0", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint32_t lastDt = 99;
    p.setMotionSender([&](const std::string& id, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t dt) {
        ++calls;
        lastDt = dt;
        REQUIRE(id == "pad-1");
    });

    GamepadInputProcessor::MotionSample s{1, 2, 3, 4, 5, 6};
    REQUIRE(p.publishMotionAt("pad-1", s, 1'000'000));
    REQUIRE(calls == 1);
    REQUIRE(lastDt == 0U);
}

TEST_CASE("publishMotionAt drops samples inside the 4 ms gate", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t) { ++calls; });

    GamepadInputProcessor::MotionSample s{};
    REQUIRE(p.publishMotionAt("pad-1", s, 1'000'000));
    // Still inside the kMotionMinIntervalUs (4 000 µs) gate.
    REQUIRE_FALSE(p.publishMotionAt("pad-1", s, 1'000'000 + 3'999));
    REQUIRE(calls == 1);
}

TEST_CASE("publishMotionAt forwards once the 4 ms gate elapses", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint32_t lastDt = 0;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t dt) {
        ++calls;
        lastDt = dt;
    });

    GamepadInputProcessor::MotionSample s{};
    REQUIRE(p.publishMotionAt("pad-1", s, 0));
    REQUIRE(p.publishMotionAt("pad-1", s, GamepadInputProcessor::kMotionMinIntervalUs));
    REQUIRE(calls == 2);
    REQUIRE(lastDt == GamepadInputProcessor::kMotionMinIntervalUs);
}

TEST_CASE("publishMotionAt does NOT advance the gate on a dropped sample", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t) { ++calls; });

    GamepadInputProcessor::MotionSample s{};
    REQUIRE(p.publishMotionAt("pad-1", s, 0));
    REQUIRE_FALSE(p.publishMotionAt("pad-1", s, 1'000));
    REQUIRE_FALSE(p.publishMotionAt("pad-1", s, 2'000));
    REQUIRE_FALSE(p.publishMotionAt("pad-1", s, 3'000));
    // The gate is measured from the last EMITTED sample; had the drops advanced
    // it, this would have been pushed out to 7 000 µs.
    REQUIRE(p.publishMotionAt("pad-1", s, 4'000));
    REQUIRE(calls == 2);
}

TEST_CASE("publishMotionAt rate-limits each device independently", "[motion]") {
    GamepadInputProcessor p;
    int aCalls = 0;
    int bCalls = 0;
    p.setMotionSender([&](const std::string& id, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t) {
        if (id == "a") { ++aCalls; }
        if (id == "b") { ++bCalls; }
    });

    GamepadInputProcessor::MotionSample s{};
    REQUIRE(p.publishMotionAt("a", s, 0));
    REQUIRE(p.publishMotionAt("b", s, 0));
    REQUIRE_FALSE(p.publishMotionAt("a", s, 1'000));
    REQUIRE_FALSE(p.publishMotionAt("b", s, 1'000));
    REQUIRE(aCalls == 1);
    REQUIRE(bCalls == 1);
}

TEST_CASE("publishMotionAt rate-limits correctly when the clock starts at 0", "[motion]") {
    // A monotonic clock can legitimately read 0 for the first sample, so the
    // gate tracks an explicit hasEmitted flag rather than testing prev != 0.
    GamepadInputProcessor p;
    int calls = 0;
    std::uint32_t lastDt = 99;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t dt) {
        ++calls;
        lastDt = dt;
    });

    GamepadInputProcessor::MotionSample s{};
    REQUIRE(p.publishMotionAt("pad-1", s, 0));
    REQUIRE(calls == 1);
    REQUIRE(lastDt == 0U);

    REQUIRE_FALSE(p.publishMotionAt("pad-1", s, 2'000));
    REQUIRE(calls == 1);

    REQUIRE(p.publishMotionAt("pad-1", s, 4'000));
    REQUIRE(calls == 2);
    REQUIRE(lastDt == 4'000U);
}

TEST_CASE("remove resets the motion rate-limit for that device", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint32_t lastDt = 99;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t, std::uint32_t dt) {
        ++calls;
        lastDt = dt;
    });

    GamepadInputProcessor::MotionSample s{};
    REQUIRE(p.publishMotionAt("pad", s, 0));
    p.remove("pad");
    REQUIRE(p.publishMotionAt("pad", s, 1));
    REQUIRE(calls == 2);
    REQUIRE(lastDt == 0U);
}

TEST_CASE("publishMotion passes through gyro + accel sample data verbatim", "[motion]") {
    GamepadInputProcessor p;
    GamepadInputProcessor::MotionSample observed{};
    bool called = false;
    p.setMotionSender([&](const std::string&, std::int16_t gx, std::int16_t gy, std::int16_t gz,
                          std::int16_t ax, std::int16_t ay, std::int16_t az, std::uint32_t) {
        observed = {gx, gy, gz, ax, ay, az};
        called = true;
    });

    GamepadInputProcessor::MotionSample s{100, -200, 300, -400, 500, -600};
    p.publishMotionAt("pad", s, 0);
    REQUIRE(called);
    REQUIRE(observed.gyroX == 100);
    REQUIRE(observed.gyroY == -200);
    REQUIRE(observed.gyroZ == 300);
    REQUIRE(observed.accelX == -400);
    REQUIRE(observed.accelY == 500);
    REQUIRE(observed.accelZ == -600);
}

TEST_CASE("publishBattery forwards the first sample", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint8_t lastLevel = 0;
    std::uint8_t lastStatus = 0;
    p.setBatterySender([&](const std::string& id, std::uint8_t l, std::uint8_t st) {
        ++calls;
        lastLevel = l;
        lastStatus = st;
        REQUIRE(id == "pad");
    });
    p.publishBattery("pad", {75, 1});
    REQUIRE(calls == 1);
    REQUIRE(lastLevel == 75);
    REQUIRE(lastStatus == 1);
}

// MSG_BATTERY is a fixed 30 s heartbeat: an unchanged value must still reach the
// wire every poll so a dropped UDP packet self-heals on the next tick.
TEST_CASE("publishBattery forwards every sample, including unchanged ones", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t) { ++calls; });

    p.publishBattery("pad", {100, 4}); // Wired, full.
    p.publishBattery("pad", {100, 4});
    p.publishBattery("pad", {100, 4});
    REQUIRE(calls == 3);
}

TEST_CASE("publishBattery emits on level change", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t) { ++calls; });

    p.publishBattery("pad", {75, 1});
    p.publishBattery("pad", {74, 1});
    p.publishBattery("pad", {73, 1});
    REQUIRE(calls == 3);
}

TEST_CASE("publishBattery emits on status change at the same level", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint8_t lastStatus = 0xFF;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t st) {
        ++calls;
        lastStatus = st;
    });

    p.publishBattery("pad", {80, 1}); // Discharging.
    p.publishBattery("pad", {80, 2}); // Charging.
    REQUIRE(calls == 2);
    REQUIRE(lastStatus == 2);
}

TEST_CASE("publishBattery forwards every device's samples", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t) { ++calls; });

    p.publishBattery("a", {50, 1});
    p.publishBattery("b", {50, 1});
    p.publishBattery("a", {50, 1});
    p.publishBattery("b", {50, 1});
    REQUIRE(calls == 4);
}

// A touchpad is an absolute surface and the SDL bridge has already assembled the
// two-finger snapshot, so publishTouchpad has no deadzone / rate-limit / coalesce
// step: it is a pure pass-through.

TEST_CASE("publishTouchpad forwards the sample to the touchpad sender", "[touchpad]") {
    GamepadInputProcessor p;
    int calls = 0;
    GamepadInputProcessor::TouchpadSample observed{};
    p.setTouchpadSender([&](const std::string& id, const GamepadInputProcessor::TouchpadSample& s) {
        ++calls;
        observed = s;
        REQUIRE(id == "pad-1");
    });

    GamepadInputProcessor::TouchpadSample sample{};
    sample.finger0Active = true;
    sample.finger0Id = 7;
    sample.finger0X = 1234;
    sample.finger0Y = -5678;
    sample.finger1Active = true;
    sample.finger1Id = 8;
    sample.finger1X = -1;
    sample.finger1Y = 32767;
    sample.buttonPressed = true;
    p.publishTouchpad("pad-1", sample);

    REQUIRE(calls == 1);
    REQUIRE(observed.finger0Active);
    REQUIRE(observed.finger0Id == 7);
    REQUIRE(observed.finger0X == 1234);
    REQUIRE(observed.finger0Y == -5678);
    REQUIRE(observed.finger1Active);
    REQUIRE(observed.finger1Id == 8);
    REQUIRE(observed.finger1X == -1);
    REQUIRE(observed.finger1Y == 32767);
    REQUIRE(observed.buttonPressed);
}

TEST_CASE("publishTouchpad forwards every sample, including unchanged ones", "[touchpad]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setTouchpadSender(
        [&](const std::string&, const GamepadInputProcessor::TouchpadSample&) { ++calls; });

    GamepadInputProcessor::TouchpadSample sample{};
    sample.finger0Active = true;
    p.publishTouchpad("pad", sample);
    p.publishTouchpad("pad", sample);
    p.publishTouchpad("pad", sample);
    REQUIRE(calls == 3);
}

TEST_CASE("publishTouchpad forwards each device's samples independently", "[touchpad]") {
    GamepadInputProcessor p;
    std::unordered_map<std::string, int> byId;
    p.setTouchpadSender(
        [&](const std::string& id, const GamepadInputProcessor::TouchpadSample&) { ++byId[id]; });

    GamepadInputProcessor::TouchpadSample sample{};
    p.publishTouchpad("a", sample);
    p.publishTouchpad("b", sample);
    p.publishTouchpad("a", sample);
    REQUIRE(byId["a"] == 2);
    REQUIRE(byId["b"] == 1);
}

TEST_CASE("publishTouchpad is a no-op when no touchpad sender is installed", "[touchpad]") {
    // A pad with a trackpad can attach before AppModel wires the sender.
    GamepadInputProcessor p;
    GamepadInputProcessor::TouchpadSample sample{};
    sample.finger0Active = true;
    p.publishTouchpad("pad", sample); // must not throw / dereference null
    SUCCEED();
}

// The per-device atomics the hot path bumps are what InputRateStore samples to
// derive Hz, so motion must count only what cleared the rate-limit gate.

TEST_CASE("inputCounters tally one gamepad event per publish", "[input][rate]") {
    GamepadInputProcessor p;
    p.setReportSender([](const std::string&, std::uint16_t, std::uint8_t, std::uint8_t,
                         std::int16_t, std::int16_t, std::int16_t, std::int16_t) {});
    REQUIRE(p.inputCounters("pad").gamepadEvents == 0);

    GamepadInputProcessor::DeviceState s;
    for (int i = 0; i < 7; ++i) {
        s.wButtons = static_cast<std::uint16_t>(i); // distinct states, still 7 publishes
        p.publish("pad", s);
    }
    const auto c = p.inputCounters("pad");
    REQUIRE(c.gamepadEvents == 7);
    REQUIRE(c.motionEvents == 0);
}

TEST_CASE("inputCounters count a gamepad event even for an unchanged state", "[input][rate]") {
    GamepadInputProcessor p;
    p.setReportSender([](const std::string&, std::uint16_t, std::uint8_t, std::uint8_t,
                         std::int16_t, std::int16_t, std::int16_t, std::int16_t) {});
    GamepadInputProcessor::DeviceState s; // identical each time
    p.publish("pad", s);
    p.publish("pad", s);
    p.publish("pad", s);
    REQUIRE(p.inputCounters("pad").gamepadEvents == 3);
}

TEST_CASE("inputCounters motion tally counts only forwarded samples", "[input][rate]") {
    GamepadInputProcessor p;
    int forwarded = 0;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t,
                          std::uint32_t) { ++forwarded; });

    const GamepadInputProcessor::MotionSample m{};
    const std::uint64_t minGap = GamepadInputProcessor::kMotionMinIntervalUs;
    REQUIRE(p.publishMotionAt("pad", m, 0));
    REQUIRE_FALSE(p.publishMotionAt("pad", m, 1));
    REQUIRE(p.publishMotionAt("pad", m, minGap));

    REQUIRE(forwarded == 2);
    const auto c = p.inputCounters("pad");
    REQUIRE(c.motionEvents == 2); // the dropped one did not count
    REQUIRE(c.gamepadEvents == 0);
}

TEST_CASE("inputCounters are independent per device", "[input][rate]") {
    GamepadInputProcessor p;
    p.setReportSender([](const std::string&, std::uint16_t, std::uint8_t, std::uint8_t,
                         std::int16_t, std::int16_t, std::int16_t, std::int16_t) {});
    GamepadInputProcessor::DeviceState s;
    p.publish("a", s);
    p.publish("a", s);
    p.publish("b", s);
    REQUIRE(p.inputCounters("a").gamepadEvents == 2);
    REQUIRE(p.inputCounters("b").gamepadEvents == 1);
    REQUIRE(p.inputCounters("ghost").gamepadEvents == 0); // never-seen device reads 0
}

TEST_CASE("remove resets a device's input counters so a re-attach re-baselines", "[input][rate]") {
    GamepadInputProcessor p;
    p.setReportSender([](const std::string&, std::uint16_t, std::uint8_t, std::uint8_t,
                         std::int16_t, std::int16_t, std::int16_t, std::int16_t) {});
    p.setMotionSender([](const std::string&, std::int16_t, std::int16_t, std::int16_t, std::int16_t,
                         std::int16_t, std::int16_t, std::uint32_t) {});
    GamepadInputProcessor::DeviceState s;
    p.publish("pad", s);
    p.publish("pad", s);
    (void)p.publishMotionAt("pad", GamepadInputProcessor::MotionSample{}, 0);
    REQUIRE(p.inputCounters("pad").gamepadEvents == 2);
    REQUIRE(p.inputCounters("pad").motionEvents == 1);

    p.remove("pad");
    const auto c = p.inputCounters("pad");
    REQUIRE(c.gamepadEvents == 0);
    REQUIRE(c.motionEvents == 0);

    p.publish("pad", s);
    REQUIRE(p.inputCounters("pad").gamepadEvents == 1);
}

// --- Actuation: "is anyone playing", as opposed to "is anything arriving". ---
// The rate counters above tally every report; these tally only reports that
// moved something, because a resting pad streams at 250 Hz forever and would
// otherwise hold the machine awake all night.

TEST_CASE("isActuation: a button change is always an actuation", "[input][activity]") {
    GamepadInputProcessor::DeviceState ref;
    GamepadInputProcessor::DeviceState next;
    next.wButtons = GamepadInputProcessor::Buttons::kA;
    REQUIRE(isActuation(ref, next));
    REQUIRE(isActuation(next, ref)); // and so is the release
    REQUIRE_FALSE(isActuation(next, next));
}

TEST_CASE("isActuation: sticks trip at the epsilon, not below it", "[input][activity]") {
    GamepadInputProcessor::DeviceState ref;
    GamepadInputProcessor::DeviceState at;
    at.lx = kActuationStickEpsilon;
    REQUIRE(isActuation(ref, at));

    GamepadInputProcessor::DeviceState under;
    under.lx = static_cast<std::int16_t>(kActuationStickEpsilon - 1);
    REQUIRE_FALSE(isActuation(ref, under));

    // The threshold is a magnitude, so it holds in both directions.
    GamepadInputProcessor::DeviceState negative;
    negative.ry = static_cast<std::int16_t>(-kActuationStickEpsilon);
    REQUIRE(isActuation(ref, negative));
}

TEST_CASE("isActuation: triggers trip at their own, smaller epsilon", "[input][activity]") {
    GamepadInputProcessor::DeviceState ref;
    GamepadInputProcessor::DeviceState at;
    at.rt = kActuationTriggerEpsilon;
    REQUIRE(isActuation(ref, at));

    GamepadInputProcessor::DeviceState under;
    under.lt = static_cast<std::uint8_t>(kActuationTriggerEpsilon - 1);
    REQUIRE_FALSE(isActuation(ref, under));
}

TEST_CASE("actuationCount: a fresh processor has seen nothing", "[input][activity]") {
    const GamepadInputProcessor p;
    REQUIRE(p.actuationCount() == 0);
}

TEST_CASE("actuationCount: a button press counts once, a repeat does not", "[input][activity]") {
    GamepadInputProcessor p;
    GamepadInputProcessor::DeviceState s;
    p.publish("pad", s); // neutral against a neutral reference
    REQUIRE(p.actuationCount() == 0);

    s.wButtons = GamepadInputProcessor::Buttons::kA;
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 1);
    p.publish("pad", s); // held down: the same state, over and over
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 1);

    s.wButtons = 0;
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 2); // the release is a deliberate act too
}

TEST_CASE("actuationCount: an axis move at the epsilon counts, under it does not",
          "[input][activity]") {
    GamepadInputProcessor p;
    GamepadInputProcessor::DeviceState s;
    s.lx = static_cast<std::int16_t>(kActuationStickEpsilon - 1);
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 0);

    GamepadInputProcessor p2;
    GamepadInputProcessor::DeviceState big;
    big.lx = kActuationStickEpsilon;
    p2.publish("pad", big);
    REQUIRE(p2.actuationCount() == 1);
}

TEST_CASE("actuationCount: a slow drift against the fixed reference eventually trips",
          "[input][activity]") {
    // The point of the separate reference: it only advances when it trips, so
    // a stick pushed gently over many reports still accumulates and counts.
    GamepadInputProcessor p;
    GamepadInputProcessor::DeviceState s;
    for (std::int16_t v = 400; v < kActuationStickEpsilon; v = static_cast<std::int16_t>(v + 400)) {
        s.lx = v;
        p.publish("pad", s);
        REQUIRE(p.actuationCount() == 0);
    }
    s.lx = kActuationStickEpsilon;
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 1);

    // The reference has now moved to where the stick is, so the next trip needs
    // another full epsilon of travel rather than repeating immediately.
    s.lx = static_cast<std::int16_t>(kActuationStickEpsilon + 400);
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 1);
    s.lx = static_cast<std::int16_t>(2 * kActuationStickEpsilon);
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 2);
}

TEST_CASE("actuationCount: a resting jittery axis never trips, however long it dithers",
          "[input][activity]") {
    // A USB-direct pad gets no deadzone profile, so its resting sticks dither
    // by a wire LSB every single report. This is the case that decides whether
    // an untouched pad can keep the machine awake indefinitely.
    GamepadInputProcessor p;
    GamepadInputProcessor::DeviceState s;
    for (int i = 0; i < 1000; ++i) {
        const auto swing = static_cast<std::int16_t>(i % 2 == 0 ? 1024 : -1024);
        s.lx = swing;
        s.ry = static_cast<std::int16_t>(-swing);
        s.lt = static_cast<std::uint8_t>(i % 2 == 0 ? kActuationTriggerEpsilon - 1 : 0);
        p.publish("pad", s);
    }
    REQUIRE(p.actuationCount() == 0);
}

TEST_CASE("actuationCount: publishTouchpad always counts", "[input][activity]") {
    GamepadInputProcessor p;
    const GamepadInputProcessor::TouchpadSample sample{}; // no threshold, no sender needed
    p.publishTouchpad("pad", sample);
    p.publishTouchpad("pad", sample);
    REQUIRE(p.actuationCount() == 2);
}

TEST_CASE("actuationCount: remove re-baselines the device's reference", "[input][activity]") {
    GamepadInputProcessor p;
    GamepadInputProcessor::DeviceState s;
    s.lx = kActuationStickEpsilon;
    p.publish("pad", s);
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 1);

    p.remove("pad");
    // Re-attached at the same deflection: measured against a fresh neutral
    // reference, that reads as a real move again.
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 2);
}

TEST_CASE("actuationCount: is process-wide, not per device", "[input][activity]") {
    GamepadInputProcessor p;
    GamepadInputProcessor::DeviceState s;
    s.wButtons = GamepadInputProcessor::Buttons::kB;
    p.publish("pad-a", s);
    p.publish("pad-b", s);
    REQUIRE(p.actuationCount() == 2);
    // But the references are per device, so a repeat on either is still quiet.
    p.publish("pad-a", s);
    p.publish("pad-b", s);
    REQUIRE(p.actuationCount() == 2);
}

TEST_CASE("actuationCount: the deadzone filter runs before the threshold", "[input][activity]") {
    // Actuation is measured on the filtered report, so a stick inside the
    // configured flat is zeroed and cannot trip anything.
    GamepadInputProcessor p;
    p.setDeadzones("pad", GamepadInputProcessor::Deadzones{8000, 20});
    GamepadInputProcessor::DeviceState s;
    s.lx = 7000; // well over the epsilon, but inside the flat
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 0);

    s.lx = 20000;
    p.publish("pad", s);
    REQUIRE(p.actuationCount() == 1);
}
