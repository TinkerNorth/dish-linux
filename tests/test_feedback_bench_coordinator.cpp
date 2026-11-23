// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The bench's runs, every flow: what each kind of test drives, when it ends, what its end puts
// back, and the one answer it takes.

#include "FakeAudioGateway.h"

#include "composer/FeedbackBenchCoordinator.h"
#include "core/reducer/BenchSteps.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using dish::composer::FeedbackBenchCoordinator;
using dish::reducer::BenchSurface;
using dish::reducer::BenchTest;
using dish::reducer::SurfaceWrite;
using dish::reducer::TestBuzz;
using dish::reducer::TestTriggerBuzz;

namespace {

const QString kSlot = QStringLiteral("sdl:7");

// Everything the bench asked the router for, in order.
class RecordingActuators final : public dish::composer::BenchActuators {
  public:
    struct Call {
        std::string what;
        BenchSurface surface = BenchSurface::Lightbar;
        SurfaceWrite write{};
        TestBuzz buzz{};
        TestTriggerBuzz triggerBuzz{};
    };

    void buzz(const QString& /*slotId*/, const TestBuzz& buzz) override {
        calls.push_back(Call{"buzz", {}, {}, buzz, {}});
    }
    void triggerBuzz(const QString& /*slotId*/, const TestTriggerBuzz& buzz) override {
        calls.push_back(Call{"triggerBuzz", {}, {}, {}, buzz});
    }
    void holdSurface(const QString& /*slotId*/, BenchSurface surface) override {
        calls.push_back(Call{"hold", surface, {}, {}, {}});
    }
    void writeSurface(const QString& /*slotId*/, const SurfaceWrite& write) override {
        calls.push_back(Call{"write", write.surface, write, {}, {}});
    }
    void giveBackSurface(const QString& /*slotId*/, BenchSurface surface) override {
        calls.push_back(Call{"giveBack", surface, {}, {}, {}});
    }
    dish::audio::PadAudioRoute audioRouteFor(const QString& /*slotId*/) const override {
        return route;
    }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const Call& call : calls) { out.push_back(call.what); }
        return out;
    }

    std::vector<Call> calls;
    dish::audio::PadAudioRoute route;
};

// Time stands still until the test moves it; whatever falls due on the way runs in order.
class ManualClock final : public dish::composer::BenchClock {
  public:
    std::int64_t nowMs() const override { return now_; }
    void after(std::int64_t delayMs, std::function<void()> due) override {
        pending_.push_back(Pending{now_ + delayMs, order_++, std::move(due)});
    }

    void advanceBy(std::int64_t ms) {
        const std::int64_t until = now_ + ms;
        for (;;) {
            const auto next = std::min_element(pending_.begin(), pending_.end(), &dueFirst);
            if (next == pending_.end() || next->atMs > until) { break; }
            Pending ready = std::move(*next);
            pending_.erase(next);
            now_ = ready.atMs;
            ready.due();
        }
        now_ = until;
    }

  private:
    struct Pending {
        std::int64_t atMs = 0;
        std::uint64_t order = 0;
        std::function<void()> due;
    };
    static bool dueFirst(const Pending& a, const Pending& b) {
        return a.atMs != b.atMs ? a.atMs < b.atMs : a.order < b.order;
    }

    std::int64_t now_ = 1'000'000;
    std::uint64_t order_ = 0;
    std::vector<Pending> pending_;
};

// The coordinator over the recording router, the manual clock and the fake audio stack.
struct Bench {
    RecordingActuators actuators;
    dish::test::FakeAudioGateway audio;
    dish::source::PadTestOutcomeStore outcomes;
    ManualClock clock;
    FeedbackBenchCoordinator coordinator{&actuators, &audio, &outcomes, &clock};

    // A DualSense's own 4-channel endpoint and microphone.
    void routeToPadEndpoint() {
        actuators.route.speaker = true;
        actuators.route.haptics = true;
        actuators.route.microphone = true;
        actuators.route.playbackDeviceName = "DualSense Wireless Controller";
        actuators.route.captureDeviceName = "DualSense Wireless Controller Mic";
        actuators.route.playbackChannels = 4;
    }
};

int loudestOn(const std::vector<std::int16_t>& samples, std::size_t channel, std::size_t stride) {
    int loudest = 0;
    for (std::size_t at = channel; at < samples.size(); at += stride) {
        loudest = std::max(loudest, std::abs(static_cast<int>(samples[at])));
    }
    return loudest;
}

} // namespace

TEST_CASE("a rumble test runs the named motor, stops it when it ends, and asks once",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::RumbleWeak);
    REQUIRE(bench.actuators.calls.size() == 1U);
    CHECK(bench.actuators.calls[0].buzz == TestBuzz{0, 48000, 400});
    CHECK(bench.coordinator.runFor(kSlot).running == BenchTest::RumbleWeak);
    CHECK_FALSE(bench.coordinator.runFor(kSlot).asking.has_value());

    bench.clock.advanceBy(400);
    REQUIRE(bench.actuators.calls.size() == 2U);
    CHECK(bench.actuators.calls[1].buzz == TestBuzz{});
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::RumbleWeak);

    bench.clock.advanceBy(5000);
    bench.coordinator.answer(kSlot, true);
    const auto outcome = bench.outcomes.lastFor(kSlot.toStdString());
    REQUIRE(outcome.has_value());
    CHECK(outcome->test == "rumble.weak");
    CHECK(outcome->result == "worked");
    CHECK(outcome->atMs == 1'000'400);
    bench.coordinator.answer(kSlot, false);
    CHECK(bench.outcomes.outcomesFor(kSlot.toStdString()).size() == 1U);
}

TEST_CASE("a trigger rumble test runs the named trigger motor and stops it",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::TriggerRumbleRight);
    bench.clock.advanceBy(400);
    REQUIRE(bench.actuators.names() == std::vector<std::string>{"triggerBuzz", "triggerBuzz"});
    CHECK(bench.actuators.calls[0].triggerBuzz == TestTriggerBuzz{0, 48000, 400});
    CHECK(bench.actuators.calls[1].triggerBuzz == TestTriggerBuzz{});
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::TriggerRumbleRight);
}

TEST_CASE("a light bar test holds the bar, shows each colour on its step, and gives it back",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::LightbarCycle);
    CHECK(bench.actuators.names() == std::vector<std::string>{"hold", "write"});
    bench.clock.advanceBy(399);
    CHECK(bench.actuators.calls.size() == 2U);
    bench.clock.advanceBy(1);
    bench.clock.advanceBy(400);
    CHECK(bench.actuators.names() == std::vector<std::string>{"hold", "write", "write", "write"});
    CHECK(bench.actuators.calls[3].write.values.lightbar ==
          dish::reducer::LightbarValue{0, 0, 0xFF});
    CHECK(bench.coordinator.runFor(kSlot).running == BenchTest::LightbarCycle);
    bench.clock.advanceBy(400);
    REQUIRE(bench.actuators.calls.size() == 5U);
    CHECK(bench.actuators.calls[4].what == "giveBack");
    CHECK(bench.actuators.calls[4].surface == BenchSurface::Lightbar);
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::LightbarCycle);
}

TEST_CASE("the trigger pulse and the lamp tests hold their surface for 2 s",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::LampPulse);
    bench.clock.advanceBy(1999);
    CHECK(bench.actuators.names() == std::vector<std::string>{"hold", "write"});
    CHECK(bench.actuators.calls[1].write.values.micLamp == dish::proto::kMicLedStatePulse);
    bench.clock.advanceBy(1);
    CHECK(bench.actuators.names() == std::vector<std::string>{"hold", "write", "giveBack"});
    CHECK(bench.actuators.calls[2].surface == BenchSurface::MicLamp);
}

TEST_CASE("a test started while another runs gives the first one's surface back first, and only "
          "the new one is asked about",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::LightbarCycle);
    bench.clock.advanceBy(400);
    bench.coordinator.start(kSlot, BenchTest::TriggerPulse);
    const std::vector<std::string> handOver{"hold", "write", "write", "giveBack", "hold", "write"};
    CHECK(bench.actuators.names() == handOver);
    CHECK(bench.actuators.calls[3].surface == BenchSurface::Lightbar);
    CHECK(bench.actuators.calls[4].surface == BenchSurface::TriggerEffects);
    // The first test's later step and its end fall due and find it replaced.
    bench.clock.advanceBy(800);
    CHECK(bench.actuators.calls.size() == handOver.size());
    CHECK(bench.coordinator.runFor(kSlot).running == BenchTest::TriggerPulse);
    bench.clock.advanceBy(1200);
    CHECK(bench.actuators.calls.back().what == "giveBack");
    CHECK(bench.actuators.calls.back().surface == BenchSurface::TriggerEffects);
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::TriggerPulse);
}

TEST_CASE("a test on another pad leaves this pad's run alone", "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::RumbleBoth);
    bench.coordinator.start(QStringLiteral("sdl:8"), BenchTest::RumbleStrong);
    bench.clock.advanceBy(400);
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::RumbleBoth);
    CHECK(bench.coordinator.runFor(QStringLiteral("sdl:8")).asking == BenchTest::RumbleStrong);
}

TEST_CASE("the speaker's tone plays on the pad's own endpoint, on the speaker pair alone",
          "[diagnostics][bench]") {
    Bench bench;
    bench.routeToPadEndpoint();
    bench.coordinator.start(kSlot, BenchTest::TonePlay);
    auto* playback = bench.audio.playbackFor("DualSense Wireless Controller");
    REQUIRE(playback != nullptr);
    CHECK(playback->channels == 4);
    CHECK(playback->resumed);
    CHECK(playback->queued.size() == static_cast<std::size_t>(38400 * 4));
    CHECK(loudestOn(playback->queued, 0, 4) > 0);
    CHECK(loudestOn(playback->queued, 2, 4) == 0);
    CHECK(loudestOn(playback->queued, 3, 4) == 0);
    bench.clock.advanceBy(800);
    CHECK(bench.audio.openPlaybackCount() == 1);
    bench.clock.advanceBy(120);
    CHECK(bench.audio.openPlaybackCount() == 0);
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::TonePlay);
}

TEST_CASE("the haptics tone plays on the actuator pair alone", "[diagnostics][bench]") {
    Bench bench;
    bench.routeToPadEndpoint();
    bench.coordinator.start(kSlot, BenchTest::HapticsPlay);
    auto* playback = bench.audio.playbackFor("DualSense Wireless Controller");
    REQUIRE(playback != nullptr);
    CHECK(loudestOn(playback->queued, 0, 4) == 0);
    CHECK(loudestOn(playback->queued, 1, 4) == 0);
    CHECK(loudestOn(playback->queued, 2, 4) > 0);
    CHECK(loudestOn(playback->queued, 3, 4) > 0);
}

// The offer gates haptics.play on the haptics lane (slotCarriesHapticPlayout), so the run does too:
// a speaker endpoint that carries no actuator lanes plays no haptics tone.
TEST_CASE("the haptics tone runs only where the haptics lane is", "[diagnostics][bench]") {
    Bench bench;
    bench.routeToPadEndpoint();
    bench.actuators.route.haptics = false;
    bench.coordinator.start(kSlot, BenchTest::HapticsPlay);
    CHECK(bench.audio.openPlaybackCount() == 0);
    CHECK(bench.coordinator.unavailableFor(kSlot) == BenchTest::HapticsPlay);
    SECTION("while the speaker's tone still plays there") {
        bench.coordinator.start(kSlot, BenchTest::TonePlay);
        CHECK(bench.audio.openPlaybackCount() == 1);
    }
}

TEST_CASE("a tone whose endpoint refuses to open ends at once and asks nothing",
          "[diagnostics][bench]") {
    Bench bench;
    bench.routeToPadEndpoint();
    bench.audio.refuse.push_back("DualSense Wireless Controller");
    bench.coordinator.start(kSlot, BenchTest::TonePlay);
    CHECK_FALSE(bench.coordinator.runFor(kSlot).running.has_value());
    CHECK_FALSE(bench.coordinator.runFor(kSlot).asking.has_value());
    CHECK(bench.coordinator.unavailableFor(kSlot) == BenchTest::TonePlay);
    bench.coordinator.start(kSlot, BenchTest::RumbleWeak);
    CHECK_FALSE(bench.coordinator.unavailableFor(kSlot).has_value());
}

TEST_CASE("the microphone test shows the pad's level live and closes its capture after 8 s",
          "[diagnostics][bench]") {
    Bench bench;
    bench.routeToPadEndpoint();
    bench.coordinator.start(kSlot, BenchTest::MicLevel);
    auto* capture = bench.audio.captureFor("DualSense Wireless Controller Mic");
    REQUIRE(capture != nullptr);
    std::vector<std::int16_t> loud(960);
    for (std::size_t i = 0; i < loud.size(); ++i) {
        loud[i] = (i % 2 == 0) ? std::int16_t{-32768} : std::int16_t{32767};
    }
    capture->onSamples(loud.data(), loud.size());
    CHECK(dish::audio::micPercentOf(bench.coordinator.micMeterFor(kSlot).meter) == 100);
    CHECK(bench.coordinator.micMeterFor(kSlot).peak == 1.0F);
    bench.clock.advanceBy(7999);
    CHECK(bench.audio.openCaptureCount() == 1);
    bench.clock.advanceBy(1);
    CHECK(bench.audio.openCaptureCount() == 0);
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::MicLevel);
    CHECK(bench.coordinator.micMeterFor(kSlot) == dish::audio::MicMeter{});
}

TEST_CASE("the microphone test stopped early ends then and asks", "[diagnostics][bench]") {
    Bench bench;
    bench.routeToPadEndpoint();
    bench.coordinator.start(kSlot, BenchTest::MicLevel);
    bench.clock.advanceBy(3000);
    bench.coordinator.endNow(kSlot);
    CHECK(bench.audio.openCaptureCount() == 0);
    CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::MicLevel);
    bench.coordinator.answer(kSlot, false);
    const auto outcome = bench.outcomes.lastFor(kSlot.toStdString());
    REQUIRE(outcome.has_value());
    CHECK(outcome->test == "mic.level");
    CHECK(outcome->result == "didNotWork");
    CHECK(outcome->atMs == 1'003'000);
}

TEST_CASE("a pad with no route to its own microphone records nothing it could not hear",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::MicLevel);
    CHECK(bench.audio.openCaptureCount() == 0);
    CHECK(bench.coordinator.unavailableFor(kSlot) == BenchTest::MicLevel);
    CHECK_FALSE(bench.coordinator.runFor(kSlot).asking.has_value());
}

TEST_CASE("a stick test is a run the inspector's bench ends, and it is asked about",
          "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::StickDrift);
    CHECK(bench.actuators.calls.empty());
    CHECK(bench.coordinator.runFor(kSlot).running == BenchTest::StickDrift);
    bench.clock.advanceBy(3000);
    bench.coordinator.endNow(kSlot);
    bench.coordinator.answer(kSlot, true);
    const auto outcome = bench.outcomes.lastFor(kSlot.toStdString());
    REQUIRE(outcome.has_value());
    CHECK(outcome->test == "stick.drift");
    CHECK(outcome->atMs == 1'003'000);
}

TEST_CASE("a stick test the inspector lets go of is never asked about", "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::StickRange);
    bench.coordinator.cancel(kSlot, BenchTest::StickRange);
    CHECK_FALSE(bench.coordinator.runFor(kSlot).running.has_value());
    CHECK_FALSE(bench.coordinator.runFor(kSlot).asking.has_value());
}

// The inspector's stick capture runs on its own clock; a light-bar test started while it ran
// replaced the stick run, and the capture's end must not cut that test short and ask about it.
TEST_CASE("a stick test's end leaves a test started meanwhile running", "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.start(kSlot, BenchTest::StickDrift);
    bench.coordinator.start(kSlot, BenchTest::LightbarCycle);
    bench.coordinator.endTest(kSlot, BenchTest::StickDrift);
    CHECK(bench.coordinator.runFor(kSlot).running == BenchTest::LightbarCycle);
    CHECK_FALSE(bench.coordinator.runFor(kSlot).asking.has_value());
    SECTION("and so does letting go of the stick test") {
        bench.coordinator.cancel(kSlot, BenchTest::StickDrift);
        CHECK(bench.coordinator.runFor(kSlot).running == BenchTest::LightbarCycle);
    }
    SECTION("while the stick test's own end still ends it") {
        bench.coordinator.start(kSlot, BenchTest::StickDrift);
        bench.coordinator.endTest(kSlot, BenchTest::StickDrift);
        CHECK(bench.coordinator.runFor(kSlot).asking == BenchTest::StickDrift);
    }
}

TEST_CASE("an answer with no ended test records nothing", "[diagnostics][bench]") {
    Bench bench;
    bench.coordinator.answer(kSlot, true);
    bench.coordinator.start(kSlot, BenchTest::RumbleWeak);
    bench.coordinator.answer(kSlot, true);
    CHECK(bench.outcomes.outcomesFor(kSlot.toStdString()).empty());
}

TEST_CASE("a test still running when the bench goes gives its surface back",
          "[diagnostics][bench]") {
    RecordingActuators actuators;
    dish::test::FakeAudioGateway audio;
    dish::source::PadTestOutcomeStore outcomes;
    ManualClock clock;
    {
        FeedbackBenchCoordinator coordinator{&actuators, &audio, &outcomes, &clock};
        coordinator.start(kSlot, BenchTest::PlayerLedsCycle);
    }
    CHECK(actuators.names() == std::vector<std::string>{"hold", "write", "giveBack"});
    CHECK(actuators.calls.back().surface == BenchSurface::PlayerLeds);
}
