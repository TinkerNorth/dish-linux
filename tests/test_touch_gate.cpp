// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Direct touchpad gate, case for case with dish-android's TouchpadGate tests: each case is the
// reports a pad sends, the clock they arrive at, and what the gate answers.

#include "Input/TouchGate.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using dish::input::GamepadInputProcessor;
using dish::input::isTouchpadEdge;
using dish::input::kTouchpadHealResends;
using dish::input::kTouchpadMoveIntervalNs;
using dish::input::TouchGate;
using dish::input::TouchpadSend;

namespace {

using Sample = GamepadInputProcessor::TouchpadSample;

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kInterval = kTouchpadMoveIntervalNs;

Sample touchAt(std::int16_t x, std::int16_t y, bool click = false, std::uint8_t id = 1) {
    Sample t;
    t.finger0Active = true;
    t.finger0Id = id;
    t.finger0X = x;
    t.finger0Y = y;
    t.buttonPressed = click;
    return t;
}

// The event time is checked only when the step expects a send.
struct Step {
    Sample sample;
    std::int64_t nowNs;
    TouchpadSend send;
    std::uint32_t eventTimeMs;
};

void runSteps(const std::vector<Step>& steps) {
    TouchGate gate;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        INFO("step " << i);
        const auto verdict = gate.decide(steps[i].sample, steps[i].nowNs);
        CHECK(verdict.send == steps[i].send);
        if (steps[i].send != TouchpadSend::Skip) {
            CHECK(verdict.eventTimeMs == steps[i].eventTimeMs);
        }
    }
}

Sample twoFingers() {
    Sample s = touchAt(100, 200, false, 3);
    s.finger1Active = true;
    s.finger1Id = 4;
    s.finger1X = 300;
    s.finger1Y = 400;
    return s;
}

struct FieldChange {
    const char* field;
    void (*apply)(Sample&);
    bool isEdge;
    bool isSameTouch;
};

std::vector<FieldChange> everyFieldChange() {
    return {
        {"finger0Active", [](Sample& s) { s.finger0Active = !s.finger0Active; }, true, false},
        {"finger1Active", [](Sample& s) { s.finger1Active = !s.finger1Active; }, true, false},
        {"buttonPressed", [](Sample& s) { s.buttonPressed = !s.buttonPressed; }, true, false},
        {"finger0Id", [](Sample& s) { ++s.finger0Id; }, true, false},
        {"finger1Id", [](Sample& s) { ++s.finger1Id; }, true, false},
        {"finger0X", [](Sample& s) { ++s.finger0X; }, false, false},
        {"finger0Y", [](Sample& s) { ++s.finger0Y; }, false, false},
        {"finger1X", [](Sample& s) { ++s.finger1X; }, false, false},
        {"finger1Y", [](Sample& s) { ++s.finger1Y; }, false, false},
        {"eventTimeMs", [](Sample& s) { s.eventTimeMs += 1000; }, false, true},
        {"rightPressed", [](Sample& s) { s.rightPressed = !s.rightPressed; }, true, false},
        {"scrollV", [](Sample& s) { s.scrollV = 120; }, true, false},
    };
}

} // namespace

TEST_CASE("the touch gate's numbers are dish-android's", "[touch-gate]") {
    STATIC_REQUIRE(kTouchpadMoveIntervalNs == 8 * kMs);
    STATIC_REQUIRE(kTouchpadHealResends == 2);
}

TEST_CASE("an idle touchpad sends nothing", "[touch-gate]") {
    runSteps({
        {Sample{}, 1000, TouchpadSend::Skip, 0},
        {Sample{}, 1000 + kInterval, TouchpadSend::Skip, 0},
    });
}

TEST_CASE("the first contact is an edge and goes at once", "[touch-gate]") {
    runSteps({{touchAt(10, 10), 5 * kMs, TouchpadSend::Fresh, 5}});
}

TEST_CASE("moves coalesce to the interval, and the newest goes", "[touch-gate]") {
    runSteps({
        {touchAt(0, 0), 0, TouchpadSend::Fresh, 0},
        {touchAt(5, 5), 1 * kMs, TouchpadSend::Skip, 0},
        {touchAt(9, 9), kInterval, TouchpadSend::Fresh, 8},
        {touchAt(9, 9), 2 * kInterval, TouchpadSend::Heal, 8},
    });
}

TEST_CASE("a coalesced move is not taken as sent", "[touch-gate]") {
    runSteps({
        {touchAt(0, 0), 0, TouchpadSend::Fresh, 0},
        {touchAt(5, 5), 1 * kMs, TouchpadSend::Skip, 0},
        {touchAt(0, 0), kInterval, TouchpadSend::Heal, 0},
    });
}

TEST_CASE("a click and a lift go at once, inside the move interval", "[touch-gate]") {
    runSteps({
        {touchAt(0, 0), 0, TouchpadSend::Fresh, 0},
        {touchAt(0, 0, true), 1 * kMs, TouchpadSend::Fresh, 1},
        {Sample{}, 2 * kMs, TouchpadSend::Fresh, 2},
    });
}

TEST_CASE("a new tracking id is an edge", "[touch-gate]") {
    runSteps({
        {touchAt(0, 0, false, 1), 0, TouchpadSend::Fresh, 0},
        {touchAt(0, 0, false, 2), 1 * kMs, TouchpadSend::Fresh, 1},
    });
}

TEST_CASE("an unchanged frame heals twice, an interval apart, with the original's event time",
          "[touch-gate]") {
    runSteps({
        {touchAt(3, 4), 0, TouchpadSend::Fresh, 0},
        {touchAt(3, 4), kInterval - 1, TouchpadSend::Skip, 0},
        {touchAt(3, 4), kInterval, TouchpadSend::Heal, 0},
        {touchAt(3, 4), 2 * kInterval - 1, TouchpadSend::Skip, 0},
        {touchAt(3, 4), 2 * kInterval, TouchpadSend::Heal, 0},
        {touchAt(3, 4), 3 * kInterval, TouchpadSend::Skip, 0},
        {touchAt(3, 4), 100 * kInterval, TouchpadSend::Skip, 0},
    });
}

TEST_CASE("a change re-arms the heal budget", "[touch-gate]") {
    runSteps({
        {touchAt(3, 4), 0, TouchpadSend::Fresh, 0},
        {touchAt(3, 4), kInterval, TouchpadSend::Heal, 0},
        {Sample{}, 2 * kInterval, TouchpadSend::Fresh, 16},
        {Sample{}, 3 * kInterval, TouchpadSend::Heal, 16},
        {Sample{}, 4 * kInterval, TouchpadSend::Heal, 16},
        {Sample{}, 5 * kInterval, TouchpadSend::Skip, 0},
    });
}

TEST_CASE("the event time is the clock in milliseconds, cut to the wire's 32 bits",
          "[touch-gate]") {
    const std::int64_t pastTheWrap = ((std::int64_t{1} << 32) + 5) * kMs;
    runSteps({{touchAt(1, 1), pastTheWrap, TouchpadSend::Fresh, 5}});
}

TEST_CASE("a contact, the click or a tracking id is an edge; a coordinate or the time is not",
          "[touch-gate]") {
    for (const FieldChange& change : everyFieldChange()) {
        INFO(change.field);
        const Sample before = twoFingers();
        Sample after = before;
        change.apply(after);
        CHECK(isTouchpadEdge(before, after) == change.isEdge);
    }
}

TEST_CASE("a wheel turn goes at once, inside the move interval, and a still wheel is not an edge",
          "[touch-gate]") {
    Sample turning = touchAt(1, 1);
    turning.scrollV = 120;
    Sample still = touchAt(1, 1);
    runSteps({{touchAt(1, 1), 0, TouchpadSend::Fresh, 0},
              {turning, kMs, TouchpadSend::Fresh, 1},
              {still, 2 * kMs, TouchpadSend::Skip, 0},
              {turning, 3 * kMs, TouchpadSend::Fresh, 3}});
}

TEST_CASE("sameTouch covers every field but the event time", "[touch-gate]") {
    for (const FieldChange& change : everyFieldChange()) {
        INFO(change.field);
        const Sample before = twoFingers();
        Sample after = before;
        change.apply(after);
        CHECK(before.sameTouch(after) == change.isSameTouch);
        CHECK(after.sameTouch(before) == change.isSameTouch);
    }
}
