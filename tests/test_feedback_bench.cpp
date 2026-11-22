// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/BenchItems.h"
#include "core/reducer/BenchRuns.h"
#include "core/reducer/BenchSteps.h"
#include "core/reducer/FeedbackBench.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using dish::reducer::BenchItem;
using dish::reducer::BenchRun;
using dish::reducer::BenchRunKind;
using dish::reducer::BenchSurface;
using dish::reducer::BenchTest;
using dish::reducer::BuzzMotor;
using dish::reducer::kTestBuzzMagnitude;
using dish::reducer::kTestBuzzMs;
using dish::reducer::SlotFeedbackInputs;
using dish::reducer::TestBuzz;
using dish::reducer::testBuzzFor;
using dish::reducer::TestTriggerBuzz;
using dish::reducer::testTriggerBuzzFor;
using dish::reducer::TriggerMotor;

namespace {

// The contract's test names (docs/diagnostics-bundle.md, "Outcomes"), in its order.
constexpr std::array<std::string_view, 16> kContractTestNames{
    "rumble.weak",         "rumble.strong",
    "rumble.both",         "triggerRumble.left",
    "triggerRumble.right", "triggerRumble.both",
    "lightbar.cycle",      "playerLeds.cycle",
    "trigger.pulse",       "lamp.on",
    "lamp.pulse",          "tone.play",
    "haptics.play",        "mic.level",
    "stick.drift",         "stick.range"};

// A claimed Direct DualSense on USB with its own audio endpoint: every surface but trigger
// rumble.
SlotFeedbackInputs directDualSense() {
    SlotFeedbackInputs in;
    in.usbDirect = true;
    in.directClaimLive = true;
    in.padRumble = true;
    in.padLightbar = true;
    in.padTriggerEffects = true;
    in.padPlayerLeds = true;
    in.padMicLed = true;
    in.padMicRoute = true;
    in.padSpeakerRoute = true;
    in.padHapticRoute = true;
    return in;
}

// An Xbox pad on SDL whose driver drives the trigger motors: rumble and trigger rumble only.
SlotFeedbackInputs sdlImpulsePad() {
    SlotFeedbackInputs in;
    in.padRumble = true;
    in.padTriggerRumble = true;
    return in;
}

std::vector<BenchItem> offeredItems(const SlotFeedbackInputs& in) {
    std::vector<BenchItem> offered;
    for (const auto& row : dish::reducer::kBenchItemRows) {
        if (dish::reducer::benchOffers(in, row.item)) { offered.push_back(row.item); }
    }
    return offered;
}

} // namespace

TEST_CASE("a test buzz runs the motor it names, at the inspector's strength and length",
          "[diagnostics][bench]") {
    CHECK(testBuzzFor(BuzzMotor::Weak) == TestBuzz{0, kTestBuzzMagnitude, kTestBuzzMs});
    CHECK(testBuzzFor(BuzzMotor::Strong) == TestBuzz{kTestBuzzMagnitude, 0, kTestBuzzMs});
    CHECK(testBuzzFor(BuzzMotor::Both) ==
          TestBuzz{kTestBuzzMagnitude, kTestBuzzMagnitude, kTestBuzzMs});
}

TEST_CASE("the test buzz is 48000 of 65535 for 400 ms, as on every client",
          "[diagnostics][bench]") {
    CHECK(kTestBuzzMagnitude == 48000);
    CHECK(kTestBuzzMs == 400);
}

TEST_CASE("a trigger buzz runs the trigger motor it names at the body motors' strength",
          "[diagnostics][bench]") {
    CHECK(testTriggerBuzzFor(TriggerMotor::Left) == TestTriggerBuzz{48000, 0, 400});
    CHECK(testTriggerBuzzFor(TriggerMotor::Right) == TestTriggerBuzz{0, 48000, 400});
    CHECK(testTriggerBuzzFor(TriggerMotor::Both) == TestTriggerBuzz{48000, 48000, 400});
}

TEST_CASE("every bench test carries the contract's name, in the contract's order",
          "[diagnostics][bench]") {
    REQUIRE(dish::reducer::kBenchTestRows.size() == kContractTestNames.size());
    for (std::size_t i = 0; i < kContractTestNames.size(); ++i) {
        const auto& row = dish::reducer::kBenchTestRows[i];
        CHECK(static_cast<std::size_t>(row.test) == i);
        CHECK(row.name == kContractTestNames[i]);
        CHECK(dish::reducer::benchTestFrom(row.name) == row.test);
    }
}

TEST_CASE("a name the contract does not list is no bench test", "[diagnostics][bench]") {
    CHECK_FALSE(dish::reducer::benchTestFrom("lightbar").has_value());
    CHECK_FALSE(dish::reducer::benchTestFrom("weak").has_value());
    CHECK_FALSE(dish::reducer::benchTestFrom("").has_value());
}

TEST_CASE("each bench item's row sits at its own index", "[diagnostics][bench]") {
    for (std::size_t i = 0; i < dish::reducer::kBenchItemRows.size(); ++i) {
        CHECK(static_cast<std::size_t>(dish::reducer::kBenchItemRows[i].item) == i);
    }
    CHECK(dish::reducer::benchItemToken(BenchItem::AdaptiveTriggers) == "adaptiveTriggers");
    CHECK(dish::reducer::benchItemOf(BenchTest::TriggerPulse) == BenchItem::AdaptiveTriggers);
    CHECK(dish::reducer::benchItemOf(BenchTest::HapticsPlay) == BenchItem::Haptics);
}

TEST_CASE("a claimed Direct DualSense is offered every item but trigger rumble",
          "[diagnostics][bench]") {
    const std::vector<BenchItem> expected{
        BenchItem::Rumble,           BenchItem::Lightbar,   BenchItem::PlayerLeds,
        BenchItem::AdaptiveTriggers, BenchItem::MicLamp,    BenchItem::Speaker,
        BenchItem::Haptics,          BenchItem::Microphone, BenchItem::Sticks};
    CHECK(offeredItems(directDualSense()) == expected);
}

TEST_CASE("a Direct pad whose claim went away is offered only its sticks", "[diagnostics][bench]") {
    SlotFeedbackInputs in = directDualSense();
    in.directClaimLive = false;
    in.padMicRoute = false;
    in.padSpeakerRoute = false;
    in.padHapticRoute = false;
    CHECK(offeredItems(in) == std::vector<BenchItem>{BenchItem::Sticks});
}

TEST_CASE("the Direct claim is never offered trigger rumble, whatever the pad has",
          "[diagnostics][bench]") {
    SlotFeedbackInputs in = directDualSense();
    in.padTriggerRumble = true;
    CHECK_FALSE(dish::reducer::benchOffers(in, BenchItem::TriggerRumble));
}

TEST_CASE("an SDL pad whose driver drives the trigger motors is offered trigger rumble",
          "[diagnostics][bench]") {
    const std::vector<BenchItem> expected{BenchItem::Rumble, BenchItem::TriggerRumble,
                                          BenchItem::Sticks};
    CHECK(offeredItems(sdlImpulsePad()) == expected);
}

TEST_CASE("an SDL DualSense is offered its effect surfaces only where SDL takes the effect body",
          "[diagnostics][bench]") {
    SlotFeedbackInputs in;
    in.padRumble = true;
    in.padLightbar = true;
    in.padTriggerEffects = true;
    in.padPlayerLeds = true;
    in.padMicLed = true;
    const std::vector<BenchItem> withoutTheBody{BenchItem::Rumble, BenchItem::Lightbar,
                                                BenchItem::Sticks};
    CHECK(offeredItems(in) == withoutTheBody);
    in.standardEffects = true;
    const std::vector<BenchItem> withTheBody{BenchItem::Rumble,     BenchItem::Lightbar,
                                             BenchItem::PlayerLeds, BenchItem::AdaptiveTriggers,
                                             BenchItem::MicLamp,    BenchItem::Sticks};
    CHECK(offeredItems(in) == withTheBody);
}

// Both desktop paths that carry the player LEDs write the mask byte themselves: the claim's OUT
// report, and the effect body SDL_GameControllerSendEffect takes. Neither lights them by a player
// number (SDL_GameControllerSetPlayerIndex is not used, tests/PARITY.md section 5b), so the test is
// the contract's five masks on both, and an SDL pad SDL cannot hand the effect body is not offered
// it at all.
TEST_CASE("every desktop path that carries the player LEDs is offered the five masks",
          "[diagnostics][bench]") {
    SlotFeedbackInputs sdlWithTheBody;
    sdlWithTheBody.padPlayerLeds = true;
    sdlWithTheBody.standardEffects = true;
    SlotFeedbackInputs sdlWithoutTheBody = sdlWithTheBody;
    sdlWithoutTheBody.standardEffects = false;
    CHECK(dish::reducer::benchOffers(directDualSense(), BenchItem::PlayerLeds));
    CHECK(dish::reducer::benchOffers(sdlWithTheBody, BenchItem::PlayerLeds));
    CHECK_FALSE(dish::reducer::benchOffers(sdlWithoutTheBody, BenchItem::PlayerLeds));
    std::vector<int> masks;
    for (std::size_t step = 0; step < dish::reducer::kPlayerLedCycle.size(); ++step) {
        masks.push_back(
            dish::reducer::surfaceStepFor(BenchTest::PlayerLedsCycle, step).values.playerLeds);
    }
    CHECK(masks == std::vector<int>{0x01, 0x02, 0x04, 0x08, 0x10});
}

TEST_CASE("the haptics tone is offered only where the pad's endpoint has the actuator pair",
          "[diagnostics][bench]") {
    SlotFeedbackInputs in;
    in.padSpeakerRoute = true;
    CHECK(dish::reducer::benchOffers(in, BenchItem::Speaker));
    CHECK_FALSE(dish::reducer::benchOffers(in, BenchItem::Haptics));
    in.padHapticRoute = true;
    CHECK(dish::reducer::benchOffers(in, BenchItem::Haptics));
}

TEST_CASE("a pad with no route to its own microphone is not offered the level test",
          "[diagnostics][bench]") {
    SlotFeedbackInputs in;
    CHECK_FALSE(dish::reducer::benchOffers(in, BenchItem::Microphone));
    in.padMicRoute = true;
    CHECK(dish::reducer::benchOffers(in, BenchItem::Microphone));
}

TEST_CASE("each test runs as its item's kind of run", "[diagnostics][bench]") {
    using dish::reducer::benchRunKindOf;
    CHECK(benchRunKindOf(BenchTest::RumbleBoth) == BenchRunKind::Buzz);
    CHECK(benchRunKindOf(BenchTest::TriggerRumbleLeft) == BenchRunKind::TriggerBuzz);
    CHECK(benchRunKindOf(BenchTest::LightbarCycle) == BenchRunKind::Surface);
    CHECK(benchRunKindOf(BenchTest::PlayerLedsCycle) == BenchRunKind::Surface);
    CHECK(benchRunKindOf(BenchTest::TriggerPulse) == BenchRunKind::Surface);
    CHECK(benchRunKindOf(BenchTest::LampPulse) == BenchRunKind::Surface);
    CHECK(benchRunKindOf(BenchTest::TonePlay) == BenchRunKind::Tone);
    CHECK(benchRunKindOf(BenchTest::HapticsPlay) == BenchRunKind::Tone);
    CHECK(benchRunKindOf(BenchTest::MicLevel) == BenchRunKind::MicLevel);
    CHECK(benchRunKindOf(BenchTest::StickRange) == BenchRunKind::Stick);
}

TEST_CASE("a buzz test names its motors, and no other test does", "[diagnostics][bench]") {
    CHECK(dish::reducer::buzzMotorOf(BenchTest::RumbleWeak) == BuzzMotor::Weak);
    CHECK(dish::reducer::buzzMotorOf(BenchTest::RumbleStrong) == BuzzMotor::Strong);
    CHECK(dish::reducer::buzzMotorOf(BenchTest::RumbleBoth) == BuzzMotor::Both);
    CHECK_FALSE(dish::reducer::buzzMotorOf(BenchTest::TriggerRumbleBoth).has_value());
    CHECK(dish::reducer::triggerMotorOf(BenchTest::TriggerRumbleLeft) == TriggerMotor::Left);
    CHECK(dish::reducer::triggerMotorOf(BenchTest::TriggerRumbleRight) == TriggerMotor::Right);
    CHECK(dish::reducer::triggerMotorOf(BenchTest::TriggerRumbleBoth) == TriggerMotor::Both);
    CHECK_FALSE(dish::reducer::triggerMotorOf(BenchTest::RumbleBoth).has_value());
}

TEST_CASE("the light bar cycles red, green, then blue, 400 ms each", "[diagnostics][bench]") {
    const auto plan = dish::reducer::surfacePlanFor(BenchTest::LightbarCycle);
    REQUIRE(plan.has_value());
    CHECK(plan->surface == BenchSurface::Lightbar);
    CHECK(plan->steps == 3U);
    CHECK(plan->holdMs == 1200);
    using dish::reducer::LightbarValue;
    using dish::reducer::surfaceStepFor;
    CHECK(surfaceStepFor(BenchTest::LightbarCycle, 0).values.lightbar == LightbarValue{0xFF, 0, 0});
    CHECK(surfaceStepFor(BenchTest::LightbarCycle, 1).values.lightbar == LightbarValue{0, 0xFF, 0});
    CHECK(surfaceStepFor(BenchTest::LightbarCycle, 2).values.lightbar == LightbarValue{0, 0, 0xFF});
    CHECK(dish::reducer::benchStepAtMs(2) == 800);
}

TEST_CASE("the player LEDs light 01, 02, 04, 08 then 10, 400 ms each", "[diagnostics][bench]") {
    const auto plan = dish::reducer::surfacePlanFor(BenchTest::PlayerLedsCycle);
    REQUIRE(plan.has_value());
    CHECK(plan->surface == BenchSurface::PlayerLeds);
    CHECK(plan->holdMs == 2000);
    std::vector<int> masks;
    for (std::size_t step = 0; step < plan->steps; ++step) {
        const auto write = dish::reducer::surfaceStepFor(BenchTest::PlayerLedsCycle, step);
        CHECK(write.surface == BenchSurface::PlayerLeds);
        masks.push_back(write.values.playerLeds);
    }
    CHECK(masks == std::vector<int>{0x01, 0x02, 0x04, 0x08, 0x10});
}

TEST_CASE("the trigger pulse holds both triggers at full rigid resistance for 2 s",
          "[diagnostics][bench]") {
    const auto plan = dish::reducer::surfacePlanFor(BenchTest::TriggerPulse);
    REQUIRE(plan.has_value());
    CHECK(plan->surface == BenchSurface::TriggerEffects);
    CHECK(plan->steps == 1U);
    CHECK(plan->holdMs == 2000);
    const auto blocks =
        dish::reducer::surfaceStepFor(BenchTest::TriggerPulse, 0).values.triggerEffects;
    dish::reducer::TriggerBlock rigid{};
    rigid[0] = 0x01;
    rigid[2] = 0xFF;
    CHECK(blocks.left == rigid);
    CHECK(blocks.right == rigid);
}

TEST_CASE("the lamp tests light the lamp on, or pulsing, for 2 s", "[diagnostics][bench]") {
    const auto on = dish::reducer::surfacePlanFor(BenchTest::LampOn);
    const auto pulse = dish::reducer::surfacePlanFor(BenchTest::LampPulse);
    REQUIRE(on.has_value());
    REQUIRE(pulse.has_value());
    CHECK(on->surface == BenchSurface::MicLamp);
    CHECK(on->holdMs == 2000);
    CHECK(pulse->holdMs == 2000);
    CHECK(dish::reducer::surfaceStepFor(BenchTest::LampOn, 0).values.micLamp ==
          dish::proto::kMicLedStateOn);
    CHECK(dish::reducer::surfaceStepFor(BenchTest::LampPulse, 0).values.micLamp ==
          dish::proto::kMicLedStatePulse);
}

TEST_CASE("only the state tests hold a surface", "[diagnostics][bench]") {
    CHECK_FALSE(dish::reducer::surfacePlanFor(BenchTest::RumbleWeak).has_value());
    CHECK_FALSE(dish::reducer::surfacePlanFor(BenchTest::TriggerRumbleBoth).has_value());
    CHECK_FALSE(dish::reducer::surfacePlanFor(BenchTest::TonePlay).has_value());
    CHECK_FALSE(dish::reducer::surfacePlanFor(BenchTest::MicLevel).has_value());
    CHECK_FALSE(dish::reducer::surfacePlanFor(BenchTest::StickDrift).has_value());
}

TEST_CASE("the microphone's level shows for at most 8 s", "[diagnostics][bench]") {
    CHECK(dish::reducer::kMicLevelMs == 8000);
}

TEST_CASE("a run is asked about once it ends, stamped with the time it ended",
          "[diagnostics][bench]") {
    const BenchRun started = dish::reducer::startedRun({}, BenchTest::LightbarCycle);
    CHECK(started.running == BenchTest::LightbarCycle);
    CHECK_FALSE(started.asking.has_value());
    const BenchRun ended = dish::reducer::endedRun(started, started.ticket, 5000);
    CHECK_FALSE(ended.running.has_value());
    CHECK(ended.asking == BenchTest::LightbarCycle);
    const auto answer = dish::reducer::answeredRun(ended, true);
    REQUIRE(answer.outcome.has_value());
    CHECK(answer.outcome->test == "lightbar.cycle");
    CHECK(answer.outcome->result == "worked");
    CHECK(answer.outcome->atMs == 5000);
}

TEST_CASE("a run takes one answer", "[diagnostics][bench]") {
    const BenchRun started = dish::reducer::startedRun({}, BenchTest::RumbleWeak);
    const BenchRun ended = dish::reducer::endedRun(started, started.ticket, 10);
    const auto first = dish::reducer::answeredRun(ended, false);
    REQUIRE(first.outcome.has_value());
    CHECK(first.outcome->result == "didNotWork");
    const auto second = dish::reducer::answeredRun(first.run, true);
    CHECK_FALSE(second.outcome.has_value());
}

TEST_CASE("a run still going is not asked about", "[diagnostics][bench]") {
    const BenchRun started = dish::reducer::startedRun({}, BenchTest::MicLevel);
    CHECK_FALSE(dish::reducer::answeredRun(started, true).outcome.has_value());
}

TEST_CASE("an end scheduled for a replaced run leaves the new run going", "[diagnostics][bench]") {
    const BenchRun first = dish::reducer::startedRun({}, BenchTest::RumbleWeak);
    const BenchRun second = dish::reducer::startedRun(first, BenchTest::RumbleStrong);
    CHECK_FALSE(dish::reducer::isCurrentRun(second, first.ticket));
    CHECK(dish::reducer::endedRun(second, first.ticket, 99) == second);
    CHECK(dish::reducer::isCurrentRun(second, second.ticket));
}

TEST_CASE("a test started while another awaits its answer drops that question",
          "[diagnostics][bench]") {
    const BenchRun first = dish::reducer::startedRun({}, BenchTest::LampOn);
    const BenchRun asking = dish::reducer::endedRun(first, first.ticket, 7);
    const BenchRun next = dish::reducer::startedRun(asking, BenchTest::LampPulse);
    CHECK_FALSE(next.asking.has_value());
    const BenchRun nextEnded = dish::reducer::endedRun(next, next.ticket, 9);
    const auto answer = dish::reducer::answeredRun(nextEnded, true);
    REQUIRE(answer.outcome.has_value());
    CHECK(answer.outcome->test == "lamp.pulse");
}
