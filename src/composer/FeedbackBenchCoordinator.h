// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Diagnostics bench's runs (docs/diagnostics-bundle.md, "The bench"): one
// test at a time per pad, each driven from this client alone at the
// contract's strengths and times, its borrowed state given back when it ends,
// and its "Did it work?" taken once, stamped with the time it ended. The
// motors and the state surfaces go through the router; the tone and the
// microphone through the pad's own audio endpoint, opened for the test and
// closed with it, so the bench needs no host and sends nothing anywhere.

#pragma once

#include "composer/BenchActuators.h"
#include "core/audio/BenchAudio.h"
#include "core/reducer/BenchItems.h"
#include "core/reducer/BenchRuns.h"
#include "source/audio/AudioDeviceGateway.h"
#include "source/store/PadTestOutcomeStore.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace dish::composer {

// The microphone test's level, shared by the capture thread that measures it and the GUI that
// shows it.
struct MicLevelShare {
    std::atomic<float> meter{0.0F};
    std::atomic<float> peak{0.0F};
};

class FeedbackBenchCoordinator : public QObject {
    Q_OBJECT
  public:
    FeedbackBenchCoordinator(BenchActuators* actuators, source::audio::AudioDeviceGateway* audio,
                             source::PadTestOutcomeStore* outcomes, BenchClock* clock,
                             QObject* parent = nullptr);
    ~FeedbackBenchCoordinator() override;

    FeedbackBenchCoordinator(const FeedbackBenchCoordinator&) = delete;
    FeedbackBenchCoordinator& operator=(const FeedbackBenchCoordinator&) = delete;
    FeedbackBenchCoordinator(FeedbackBenchCoordinator&&) = delete;
    FeedbackBenchCoordinator& operator=(FeedbackBenchCoordinator&&) = delete;

    // Starts the test on the slot's pad, ending whatever test the pad was running first; that
    // one is never asked about. A stick test only opens its run: the inspector's stick bench
    // measures it and says when it ends (endTest).
    void start(const QString& slotId, reducer::BenchTest test);
    // The slot's running test ends now, as if it had run its course: the microphone test the
    // person stopped early.
    void endNow(const QString& slotId);
    // `test` ends now as if it had run its course, if it is still the slot's running test: a
    // stick test the inspector finished. A test started since runs on.
    void endTest(const QString& slotId, reducer::BenchTest test);
    // `test` stops and is never asked about, if it is still the slot's running test: the
    // inspector let go of a stick test it was measuring. A test started since runs on.
    void cancel(const QString& slotId, reducer::BenchTest test);
    // The person's answer to the slot's ended test.
    void answer(const QString& slotId, bool worked);

    reducer::BenchRun runFor(const QString& slotId) const;
    // The microphone test's level as the page shows it; empty while none runs.
    audio::MicMeter micMeterFor(const QString& slotId) const;
    // The last test that could not reach the pad's endpoint (the audio stack refused it).
    std::optional<reducer::BenchTest> unavailableFor(const QString& slotId) const;

  signals:
    void benchChanged();

  private:
    struct SlotBench {
        reducer::BenchRun run;
        int playback = source::audio::kNoAudioDevice;
        int capture = source::audio::kNoAudioDevice;
        std::shared_ptr<MicLevelShare> mic;
        std::optional<reducer::BenchTest> unavailable;
    };

    // Each kind of run's start and its end, which stops what the start began.
    void startBuzz(const QString& slotId, std::uint64_t ticket, reducer::BenchTest test);
    void startTriggerBuzz(const QString& slotId, std::uint64_t ticket, reducer::BenchTest test);
    void startSurface(const QString& slotId, std::uint64_t ticket, reducer::BenchTest test);
    void startTone(const QString& slotId, std::uint64_t ticket, reducer::BenchTest test);
    void startMicLevel(const QString& slotId, std::uint64_t ticket);
    void stop(const QString& slotId, SlotBench& bench);

    void writeStep(const QString& slotId, std::uint64_t ticket, reducer::BenchTest test,
                   std::size_t step);
    void endRun(const QString& slotId, std::uint64_t ticket);
    void pollMicLevel(const QString& slotId, std::uint64_t ticket);
    // A run whose endpoint refused it ends at once and asks nothing.
    void refuse(const QString& slotId, SlotBench& bench);

    BenchActuators* const actuators_;
    source::audio::AudioDeviceGateway* const audio_;
    source::PadTestOutcomeStore* const outcomes_;
    BenchClock* const clock_;
    std::map<QString, SlotBench> benches_;
};

// The production clock: Qt's timers on the GUI thread, and the wall clock for the outcome. Its
// timers run in a context of its own, so letting the clock go cancels every one still pending:
// the owner lets it go before the coordinator whose steps they would run.
class QtBenchClock final : public BenchClock {
  public:
    std::int64_t nowMs() const override;
    void after(std::int64_t delayMs, std::function<void()> due) override;

  private:
    QObject context_;
};

} // namespace dish::composer
