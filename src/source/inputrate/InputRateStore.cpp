// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/inputrate/InputRateStore.h"

#include <QTimer>

#include <algorithm>
#include <chrono>

namespace dish::source {

InputRateStore::InputRateStore(CounterSource source)
    : arch::StateSource<SlotInputRatesMap>(SlotInputRatesMap{}), source_(std::move(source)) {}

InputRateStore::~InputRateStore() = default;

void InputRateStore::addDevice(const std::string& slotId) {
    if (slots_.find(slotId) != slots_.end()) { return; }
    slots_.emplace(slotId, SlotState{});
    // Publish the zero entry so subscribers see the new slot immediately.
    setState([&](const SlotInputRatesMap& current) {
        SlotInputRatesMap next = current;
        next[slotId] = SlotInputRates{};
        return next;
    });
}

void InputRateStore::removeDevice(const std::string& slotId) {
    const auto it = slots_.find(slotId);
    if (it == slots_.end()) { return; }
    slots_.erase(it);
    setState([&](const SlotInputRatesMap& current) {
        if (current.find(slotId) == current.end()) { return current; }
        SlotInputRatesMap next = current;
        next.erase(slotId);
        return next;
    });
}

void InputRateStore::sampleAt(std::uint64_t nowUs) {
    if (slots_.empty()) { return; }
    SlotInputRatesMap next;
    for (auto& [slotId, st] : slots_) {
        SlotInputCounters counters{};
        if (source_) { counters = source_(slotId); }

        const int gamepadHz = st.gamepad.sample(counters.gamepadEvents, nowUs);
        const int motionHz = st.motion.sample(counters.motionEvents, nowUs);

        st.rates.gamepadHz = gamepadHz;
        st.rates.gamepadPeakHz = std::max(st.rates.gamepadPeakHz, gamepadHz);
        st.rates.motionHz = motionHz;
        st.rates.motionPeakHz = std::max(st.rates.motionPeakHz, motionHz);
        next[slotId] = st.rates;
    }
    setState(std::move(next));
}

void InputRateStore::start() {
    if (timer_) { return; }
    timer_ = std::make_unique<QTimer>();
    timer_->setInterval(kDefaultSampleIntervalMs);
    QObject::connect(timer_.get(), &QTimer::timeout, timer_.get(), [this]() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        sampleAt(static_cast<std::uint64_t>(us));
    });
    timer_->start();
}

void InputRateStore::stop() {
    if (!timer_) { return; }
    timer_->stop();
    timer_.reset();
}

} // namespace dish::source
