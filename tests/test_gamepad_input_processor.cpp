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

// ---------------------------------------------------------------------------
// Per-device deadzones — mirrors the dish-mac GamepadInputProcessor tests
// and the Android per-device `flat` pipeline. Pinning these here keeps the
// wire format identical across all three clients.
// ---------------------------------------------------------------------------

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
    // After remove, a fresh publish should not pull the old deadzone — small
    // input passes through.
    GamepadInputProcessor::DeviceState s;
    s.lx = 100;
    p.publish("pad", s);
    REQUIRE(lastLx == 100);
}

// ── Motion rate limiting + dispatch ─────────────────────────────────────────

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
    REQUIRE_FALSE(p.publishMotionAt("pad-1", s, 1'000'000 + 3'999));
    REQUIRE(calls == 1);
}

TEST_CASE("publishMotionAt forwards once the 4 ms gate elapses", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint32_t lastDt = 0;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t,
                          std::uint32_t dt) { ++calls; lastDt = dt; });

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

TEST_CASE("remove resets the motion rate-limit for that device", "[motion]") {
    GamepadInputProcessor p;
    int calls = 0;
    std::uint32_t lastDt = 99;
    p.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, std::int16_t,
                          std::uint32_t dt) { ++calls; lastDt = dt; });

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

// ── Battery coalescing ─────────────────────────────────────────────────────

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

TEST_CASE("publishBattery coalesces identical samples", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t) { ++calls; });

    p.publishBattery("pad", {100, 4});
    p.publishBattery("pad", {100, 4});
    p.publishBattery("pad", {100, 4});
    REQUIRE(calls == 1);
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

    p.publishBattery("pad", {80, 1});
    p.publishBattery("pad", {80, 2});
    REQUIRE(calls == 2);
    REQUIRE(lastStatus == 2);
}

TEST_CASE("publishBattery coalesces independently per device", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t) { ++calls; });

    p.publishBattery("a", {50, 1});
    p.publishBattery("b", {50, 1});
    REQUIRE(calls == 2);
}

TEST_CASE("remove clears the battery coalesce cache", "[battery]") {
    GamepadInputProcessor p;
    int calls = 0;
    p.setBatterySender([&](const std::string&, std::uint8_t, std::uint8_t) { ++calls; });

    p.publishBattery("pad", {50, 1});
    p.remove("pad");
    p.publishBattery("pad", {50, 1});
    REQUIRE(calls == 2);
}
