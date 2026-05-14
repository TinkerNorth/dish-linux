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
                          std::int16_t lx, std::int16_t, std::int16_t, std::int16_t) {
        byId[id] = lx;
    });
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
                          std::int16_t lx, std::int16_t, std::int16_t, std::int16_t) {
        lastLx = lx;
    });
    p.setDeadzones("pad", {5000, 0});
    p.remove("pad");
    // After remove, a fresh publish should not pull the old deadzone — small
    // input passes through.
    GamepadInputProcessor::DeviceState s;
    s.lx = 100;
    p.publish("pad", s);
    REQUIRE(lastLx == 100);
}
