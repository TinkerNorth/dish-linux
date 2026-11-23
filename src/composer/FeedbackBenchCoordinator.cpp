// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/FeedbackBenchCoordinator.h"

#include "core/audio/AudioEnginePolicy.h"
#include "core/reducer/BenchSteps.h"

#include <QDateTime>
#include <QTimer>

#include <cstddef>
#include <utility>

namespace dish::composer {

namespace {

// The tone is queued whole; the run ends once the device has had time to play it out.
constexpr std::int64_t kToneDrainMs = 120;
constexpr std::int64_t kToneRunMs = audio::kToneMs + kToneDrainMs;
// How often the page's meter takes the capture thread's level.
constexpr std::int64_t kMicPollMs = 50;

// The capture callback: the samples arrive in whatever chunks the platform's period makes, the
// meter steps once per 20 ms window, and only the level leaves the capture thread.
class MicLevelTap {
  public:
    explicit MicLevelTap(std::shared_ptr<MicLevelShare> share)
        : share_(std::move(share)), windower_(std::make_shared<audio::FrameWindower>()),
          meter_(std::make_shared<audio::MicMeter>()) {}

    void operator()(const std::int16_t* samples, std::size_t count) const {
        windower_->feed(samples, count,
                        [this](const std::int16_t* window, std::size_t n) { step(window, n); });
    }

  private:
    void step(const std::int16_t* window, std::size_t count) const {
        *meter_ = audio::nextMicMeter(*meter_, audio::micReadingOf(window, count));
        share_->meter.store(meter_->meter, std::memory_order_relaxed);
        share_->peak.store(meter_->peak, std::memory_order_relaxed);
    }

    std::shared_ptr<MicLevelShare> share_;
    // The capture thread's own; shared only so the gateway may copy the tap.
    std::shared_ptr<audio::FrameWindower> windower_;
    std::shared_ptr<audio::MicMeter> meter_;
};

int toneLaneOffset(reducer::BenchTest test) {
    return reducer::benchItemOf(test) == reducer::BenchItem::Haptics ? audio::kHapticLaneOffset
                                                                     : audio::kSpeakerLaneOffset;
}

// The lane the offer gates the test on (slotCarriesSpeakerPlayout, slotCarriesHapticPlayout).
bool routeCarriesToneOf(const audio::PadAudioRoute& route, reducer::BenchTest test) {
    const bool haptics = reducer::benchItemOf(test) == reducer::BenchItem::Haptics;
    return haptics ? route.haptics : route.speaker;
}

} // namespace

FeedbackBenchCoordinator::FeedbackBenchCoordinator(BenchActuators* actuators,
                                                   source::audio::AudioDeviceGateway* audio,
                                                   source::PadTestOutcomeStore* outcomes,
                                                   BenchClock* clock, QObject* parent)
    : QObject(parent), actuators_(actuators), audio_(audio), outcomes_(outcomes), clock_(clock) {}

// A test still running when the app goes leaves its pad as the game had it.
FeedbackBenchCoordinator::~FeedbackBenchCoordinator() {
    for (auto& [slotId, bench] : benches_) {
        if (bench.run.running.has_value()) { stop(slotId, bench); }
    }
}

void FeedbackBenchCoordinator::start(const QString& slotId, reducer::BenchTest test) {
    SlotBench& bench = benches_[slotId];
    if (bench.run.running.has_value()) { stop(slotId, bench); }
    bench.run = reducer::startedRun(bench.run, test);
    bench.unavailable.reset();
    const std::uint64_t ticket = bench.run.ticket;
    switch (reducer::benchRunKindOf(test)) {
    case reducer::BenchRunKind::Buzz:
        startBuzz(slotId, ticket, test);
        break;
    case reducer::BenchRunKind::TriggerBuzz:
        startTriggerBuzz(slotId, ticket, test);
        break;
    case reducer::BenchRunKind::Surface:
        startSurface(slotId, ticket, test);
        break;
    case reducer::BenchRunKind::Tone:
        startTone(slotId, ticket, test);
        break;
    case reducer::BenchRunKind::MicLevel:
        startMicLevel(slotId, ticket);
        break;
    case reducer::BenchRunKind::Stick:
        break;
    }
    emit benchChanged();
}

void FeedbackBenchCoordinator::endNow(const QString& slotId) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end()) { return; }
    endRun(slotId, it->second.run.ticket);
}

void FeedbackBenchCoordinator::endTest(const QString& slotId, reducer::BenchTest test) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end() || it->second.run.running != test) { return; }
    endRun(slotId, it->second.run.ticket);
}

void FeedbackBenchCoordinator::cancel(const QString& slotId, reducer::BenchTest test) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end() || it->second.run.running != test) { return; }
    stop(slotId, it->second);
    it->second.run.running.reset();
    emit benchChanged();
}

void FeedbackBenchCoordinator::answer(const QString& slotId, bool worked) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end()) { return; }
    const reducer::BenchAnswer answered = reducer::answeredRun(it->second.run, worked);
    it->second.run = answered.run;
    if (answered.outcome.has_value()) {
        outcomes_->record(slotId.toStdString(), *answered.outcome);
    }
    emit benchChanged();
}

reducer::BenchRun FeedbackBenchCoordinator::runFor(const QString& slotId) const {
    const auto it = benches_.find(slotId);
    return it == benches_.end() ? reducer::BenchRun{} : it->second.run;
}

audio::MicMeter FeedbackBenchCoordinator::micMeterFor(const QString& slotId) const {
    const auto it = benches_.find(slotId);
    if (it == benches_.end() || it->second.mic == nullptr) { return {}; }
    return audio::MicMeter{it->second.mic->meter.load(std::memory_order_relaxed),
                           it->second.mic->peak.load(std::memory_order_relaxed)};
}

std::optional<reducer::BenchTest>
FeedbackBenchCoordinator::unavailableFor(const QString& slotId) const {
    const auto it = benches_.find(slotId);
    return it == benches_.end() ? std::nullopt : it->second.unavailable;
}

// The raw-HID path has no duration and runs the motors until the next write, so every buzz ends
// with an explicit stop, which SDL takes harmlessly.
void FeedbackBenchCoordinator::startBuzz(const QString& slotId, std::uint64_t ticket,
                                         reducer::BenchTest test) {
    const auto motor = reducer::buzzMotorOf(test);
    if (!motor.has_value()) { return; }
    actuators_->buzz(slotId, reducer::testBuzzFor(*motor));
    clock_->after(reducer::kTestBuzzMs, [this, slotId, ticket] { endRun(slotId, ticket); });
}

void FeedbackBenchCoordinator::startTriggerBuzz(const QString& slotId, std::uint64_t ticket,
                                                reducer::BenchTest test) {
    const auto motor = reducer::triggerMotorOf(test);
    if (!motor.has_value()) { return; }
    actuators_->triggerBuzz(slotId, reducer::testTriggerBuzzFor(*motor));
    clock_->after(reducer::kTestBuzzMs, [this, slotId, ticket] { endRun(slotId, ticket); });
}

void FeedbackBenchCoordinator::startSurface(const QString& slotId, std::uint64_t ticket,
                                            reducer::BenchTest test) {
    const auto plan = reducer::surfacePlanFor(test);
    if (!plan.has_value()) { return; }
    actuators_->holdSurface(slotId, plan->surface);
    actuators_->writeSurface(slotId, reducer::surfaceStepFor(test, 0));
    for (std::size_t step = 1; step < plan->steps; ++step) {
        clock_->after(reducer::benchStepAtMs(step), [this, slotId, ticket, test, step] {
            writeStep(slotId, ticket, test, step);
        });
    }
    clock_->after(plan->holdMs, [this, slotId, ticket] { endRun(slotId, ticket); });
}

// The tone opens the pad's own endpoint at its full width, as the playout engine's voices do,
// and writes only the lane's pair, so the speaker's tone never reaches the actuators.
void FeedbackBenchCoordinator::startTone(const QString& slotId, std::uint64_t ticket,
                                         reducer::BenchTest test) {
    SlotBench& bench = benches_[slotId];
    const audio::PadAudioRoute route = actuators_->audioRouteFor(slotId);
    const int channels =
        route.playbackChannels > 0 ? route.playbackChannels : proto::kAudioSpeakerChannels;
    const std::vector<std::int16_t> tone = audio::benchTone(channels, toneLaneOffset(test));
    const bool playable = routeCarriesToneOf(route, test) && !tone.empty();
    const int handle = playable ? audio_->openPlayback(route.playbackDeviceName, channels)
                                : source::audio::kNoAudioDevice;
    if (handle == source::audio::kNoAudioDevice) {
        refuse(slotId, bench);
        return;
    }
    bench.playback = handle;
    const bool queued = audio_->queuePlayback(handle, tone.data(), tone.size());
    if (!queued) {
        refuse(slotId, bench);
        return;
    }
    audio_->resumePlayback(handle);
    clock_->after(kToneRunMs, [this, slotId, ticket] { endRun(slotId, ticket); });
}

// Only the level leaves the capture thread: the samples are measured and dropped there.
void FeedbackBenchCoordinator::startMicLevel(const QString& slotId, std::uint64_t ticket) {
    SlotBench& bench = benches_[slotId];
    const audio::PadAudioRoute route = actuators_->audioRouteFor(slotId);
    auto share = std::make_shared<MicLevelShare>();
    const int handle = route.microphone
                           ? audio_->openCapture(route.captureDeviceName, MicLevelTap(share))
                           : source::audio::kNoAudioDevice;
    if (handle == source::audio::kNoAudioDevice) {
        refuse(slotId, bench);
        return;
    }
    bench.capture = handle;
    bench.mic = std::move(share);
    clock_->after(kMicPollMs, [this, slotId, ticket] { pollMicLevel(slotId, ticket); });
    clock_->after(reducer::kMicLevelMs, [this, slotId, ticket] { endRun(slotId, ticket); });
}

void FeedbackBenchCoordinator::pollMicLevel(const QString& slotId, std::uint64_t ticket) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end() || !reducer::isCurrentRun(it->second.run, ticket)) { return; }
    emit benchChanged();
    clock_->after(kMicPollMs, [this, slotId, ticket] { pollMicLevel(slotId, ticket); });
}

void FeedbackBenchCoordinator::writeStep(const QString& slotId, std::uint64_t ticket,
                                         reducer::BenchTest test, std::size_t step) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end() || !reducer::isCurrentRun(it->second.run, ticket)) { return; }
    actuators_->writeSurface(slotId, reducer::surfaceStepFor(test, step));
}

void FeedbackBenchCoordinator::endRun(const QString& slotId, std::uint64_t ticket) {
    const auto it = benches_.find(slotId);
    if (it == benches_.end() || !reducer::isCurrentRun(it->second.run, ticket)) { return; }
    stop(slotId, it->second);
    it->second.run = reducer::endedRun(it->second.run, ticket, clock_->nowMs());
    emit benchChanged();
}

void FeedbackBenchCoordinator::refuse(const QString& slotId, SlotBench& bench) {
    bench.unavailable = bench.run.running;
    stop(slotId, bench);
    bench.run.running.reset();
}

// What the running test's start began, undone: the motors stopped, the surface given back to its
// owner, the endpoint closed (a capture close joins, so no level arrives after it).
void FeedbackBenchCoordinator::stop(const QString& slotId, SlotBench& bench) {
    if (!bench.run.running.has_value()) { return; }
    const reducer::BenchTest test = *bench.run.running;
    switch (reducer::benchRunKindOf(test)) {
    case reducer::BenchRunKind::Buzz:
        actuators_->buzz(slotId, reducer::TestBuzz{});
        return;
    case reducer::BenchRunKind::TriggerBuzz:
        actuators_->triggerBuzz(slotId, reducer::TestTriggerBuzz{});
        return;
    case reducer::BenchRunKind::Surface:
        if (const auto plan = reducer::surfacePlanFor(test)) {
            actuators_->giveBackSurface(slotId, plan->surface);
        }
        return;
    case reducer::BenchRunKind::Tone:
        if (bench.playback != source::audio::kNoAudioDevice) {
            audio_->closePlayback(std::exchange(bench.playback, source::audio::kNoAudioDevice));
        }
        return;
    case reducer::BenchRunKind::MicLevel:
        if (bench.capture != source::audio::kNoAudioDevice) {
            audio_->closeCapture(std::exchange(bench.capture, source::audio::kNoAudioDevice));
        }
        bench.mic.reset();
        return;
    case reducer::BenchRunKind::Stick:
        return;
    }
}

std::int64_t QtBenchClock::nowMs() const { return QDateTime::currentMSecsSinceEpoch(); }

void QtBenchClock::after(std::int64_t delayMs, std::function<void()> due) {
    QTimer::singleShot(static_cast<int>(delayMs), &context_, std::move(due));
}

} // namespace dish::composer
