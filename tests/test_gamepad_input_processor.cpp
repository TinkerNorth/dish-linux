// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Input/GamepadInputProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

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
