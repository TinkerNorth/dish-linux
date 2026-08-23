// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "GamepadInputProcessor.h"

#include "core/input/Deadzones.h"

#include <chrono>
#include <cstdlib>

namespace dish::input {

void GamepadInputProcessor::setReportSender(ReportSender sender) {
    std::lock_guard<std::mutex> lock(mtx_);
    sender_ = std::move(sender);
}

void GamepadInputProcessor::setMotionSender(MotionSender sender) {
    std::lock_guard<std::mutex> lock(mtx_);
    motionSender_ = std::move(sender);
}

void GamepadInputProcessor::setBatterySender(BatterySender sender) {
    std::lock_guard<std::mutex> lock(mtx_);
    batterySender_ = std::move(sender);
}

void GamepadInputProcessor::setDeadzones(const DeviceId& id, const Deadzones& dz) {
    std::lock_guard<std::mutex> lock(mtx_);
    deadzones_[id] = dz;
}

void GamepadInputProcessor::publish(const DeviceId& id, const DeviceState& state) {
    ReportSender snapshot;
    DeviceState filtered;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        Deadzones dz{};
        if (auto it = deadzones_.find(id); it != deadzones_.end()) { dz = it->second; }
        filtered = applyDeadzones(state, dz);
        states_[id] = filtered;
        if (DeviceState& ref = activityRef_[id]; isActuation(ref, filtered)) {
            ref = filtered;
            actuations_.fetch_add(1, std::memory_order_relaxed);
        }
        ++telEvents_;
        ++telSends_;
        ++telTotalSent_;
        rateCounters_[id].gamepad.fetch_add(1, std::memory_order_relaxed);
        snapshot = sender_;
    }
    if (snapshot) {
        snapshot(id, filtered.wButtons, filtered.lt, filtered.rt, filtered.lx, filtered.ly,
                 filtered.rx, filtered.ry);
    }
}

void GamepadInputProcessor::zeroAndSendAll() {
    std::vector<DeviceId> ids;
    ReportSender snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ids.reserve(states_.size());
        for (auto& [k, v] : states_) {
            v = DeviceState{};
            ids.push_back(k);
        }
        snapshot = sender_;
    }
    if (!snapshot) { return; }
    for (const auto& id : ids) { snapshot(id, 0, 0, 0, 0, 0, 0, 0); }
}

void GamepadInputProcessor::remove(const DeviceId& id) {
    std::lock_guard<std::mutex> lock(mtx_);
    states_.erase(id);
    activityRef_.erase(id);
    deadzones_.erase(id);
    lastMotionUs_.erase(id);
    rateCounters_.erase(id);
}

GamepadInputProcessor::InputRateCounters
GamepadInputProcessor::inputCounters(const DeviceId& id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = rateCounters_.find(id);
    if (it == rateCounters_.end()) { return InputRateCounters{}; }
    return InputRateCounters{it->second.gamepad.load(std::memory_order_relaxed),
                             it->second.motion.load(std::memory_order_relaxed)};
}

bool GamepadInputProcessor::publishMotionAt(const DeviceId& id, const MotionSample& sample,
                                            std::uint64_t nowUs) {
    MotionSender snapshot;
    std::uint32_t deltaUs = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        MotionGate& gate = lastMotionUs_[id];
        if (gate.hasEmitted && nowUs - gate.lastUs < kMotionMinIntervalUs) {
            // Deliberately do NOT advance `gate`: a hot stream of dropped
            // samples would push it forward and starve the sender for longer
            // than one period.
            return false;
        }
        // Relative to the previous *emitted* packet, not the previous attempt,
        // so the receiver gets true inter-arrival timing. Delta stays 0 on the
        // first emission — the wire spec's first-packet sentinel.
        if (gate.hasEmitted) {
            const std::uint64_t d = nowUs - gate.lastUs;
            deltaUs = (d > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<std::uint32_t>(d);
        }
        gate.lastUs = nowUs;
        gate.hasEmitted = true;
        rateCounters_[id].motion.fetch_add(1, std::memory_order_relaxed);
        snapshot = motionSender_;
    }
    if (snapshot) {
        snapshot(id, sample.gyroX, sample.gyroY, sample.gyroZ, sample.accelX, sample.accelY,
                 sample.accelZ, deltaUs);
    }
    return true;
}

void GamepadInputProcessor::publishMotion(const DeviceId& id, const MotionSample& sample) {
    const auto now = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    (void)publishMotionAt(id, sample, us);
}

void GamepadInputProcessor::publishBattery(const DeviceId& id, const BatterySample& sample) {
    BatterySender snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snapshot = batterySender_;
    }
    if (snapshot) { snapshot(id, sample.level, sample.status); }
}

void GamepadInputProcessor::setTouchpadSender(TouchpadSender sender) {
    std::lock_guard<std::mutex> lock(mtx_);
    touchpadSender_ = std::move(sender);
}

void GamepadInputProcessor::publishTouchpad(const DeviceId& id, const TouchpadSample& sample) {
    TouchpadSender snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snapshot = touchpadSender_;
        // No threshold: a touchpad sample is a finger, not a poll.
        actuations_.fetch_add(1, std::memory_order_relaxed);
    }
    if (snapshot) { snapshot(id, sample); }
}

GamepadInputProcessor::TelemetrySnapshot GamepadInputProcessor::drainTelemetry() {
    std::lock_guard<std::mutex> lock(mtx_);
    TelemetrySnapshot snap{telEvents_, telSends_, telTotalSent_};
    telEvents_ = 0;
    telSends_ = 0;
    return snap;
}

// Thin forwarders to the pure core/input layer, kept so existing call sites
// stay unchanged.
std::int16_t scaleAxis(float v, float maxMagnitude) { return deadzone::scaleAxis(v, maxMagnitude); }

std::uint8_t scaleTrigger(float v) { return deadzone::scaleTrigger(v); }

GamepadInputProcessor::DeviceState applyDeadzones(const GamepadInputProcessor::DeviceState& state,
                                                  const GamepadInputProcessor::Deadzones& dz) {
    auto out = state;
    out.lx = deadzone::applyStick(out.lx, dz.stickFlat);
    out.ly = deadzone::applyStick(out.ly, dz.stickFlat);
    out.rx = deadzone::applyStick(out.rx, dz.stickFlat);
    out.ry = deadzone::applyStick(out.ry, dz.stickFlat);
    out.lt = deadzone::applyTrigger(out.lt, dz.triggerFlat);
    out.rt = deadzone::applyTrigger(out.rt, dz.triggerFlat);
    return out;
}

bool isActuation(const GamepadInputProcessor::DeviceState& ref,
                 const GamepadInputProcessor::DeviceState& next) {
    if (ref.wButtons != next.wButtons) { return true; }
    const auto axisMoved = [](std::int16_t a, std::int16_t b) {
        return std::abs(static_cast<std::int32_t>(a) - static_cast<std::int32_t>(b)) >=
               static_cast<std::int32_t>(kActuationStickEpsilon);
    };
    const auto triggerMoved = [](std::uint8_t a, std::uint8_t b) {
        return std::abs(static_cast<int>(a) - static_cast<int>(b)) >=
               static_cast<int>(kActuationTriggerEpsilon);
    };
    return axisMoved(ref.lx, next.lx) || axisMoved(ref.ly, next.ly) || axisMoved(ref.rx, next.rx) ||
           axisMoved(ref.ry, next.ry) || triggerMoved(ref.lt, next.lt) ||
           triggerMoved(ref.rt, next.rt);
}

} // namespace dish::input
