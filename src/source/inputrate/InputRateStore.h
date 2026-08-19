// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// InputRateStore — a StateSource over the per-slot live input rates. It samples
// the cumulative per-stream counters the input hot path exposes, runs each
// through the pure InputRateTracker to derive quantized Hz, and publishes the
// map. This class owns only the per-slot tracker state, the counter-source seam,
// and the sampling cadence.
//
// The sampling work is `sampleAt`, which takes "now" explicitly so tests can
// pump the loop with a fake clock; the QTimer exists only in production.

#pragma once

#include "architecture/StateSource.h"
#include "source/inputrate/SlotInputRates.h"
#include "core/reducer/InputRateTracker.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

class QTimer;

namespace dish::source {

// Monotonic within a device's attachment only; a counter going backwards (a
// re-attach) is handled by the tracker as a rebaseline.
struct SlotInputCounters {
    std::uint64_t gamepadEvents = 0;
    std::uint64_t motionEvents = 0;
};

// std::map keeps it deterministic + ==-comparable so the Observable's
// distinct-until-changed suppresses identical re-emits.
using SlotInputRatesMap = std::map<std::string, SlotInputRates>;

class InputRateStore : public arch::StateSource<SlotInputRatesMap> {
  public:
    // The seam sampled on every tick. Borrowed; outlives the store.
    using CounterSource = std::function<SlotInputCounters(const std::string& slotId)>;

    static constexpr int kDefaultSampleIntervalMs = 500;

    explicit InputRateStore(CounterSource source);
    ~InputRateStore() override;

    // Idempotent. A freshly added slot gets fresh trackers, so its first sample
    // only establishes a baseline and reports 0 Hz.
    void addDevice(const std::string& slotId);

    // Discards the slot's trackers too, so a later re-add re-baselines cleanly.
    // A no-op (no emit) if absent.
    void removeDevice(const std::string& slotId);

    // `nowUs` is microseconds on any monotonic basis. The test seam —
    // production's QTimer calls it with a steady clock.
    void sampleAt(std::uint64_t nowUs);

    // Idempotent, and no-ops without a Qt event loop (headless tests call
    // sampleAt directly).
    void start() override;
    void stop() override;

  private:
    struct SlotState {
        reducer::InputRateTracker gamepad;
        reducer::InputRateTracker motion;
        SlotInputRates rates;
    };

    CounterSource source_;
    std::map<std::string, SlotState> slots_;
    std::unique_ptr<QTimer> timer_;
};

} // namespace dish::source
