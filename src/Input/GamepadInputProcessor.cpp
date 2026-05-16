// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "GamepadInputProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    deadzones_.erase(id);
    lastMotionUs_.erase(id);
    lastBattery_.erase(id);
}

bool GamepadInputProcessor::publishMotionAt(const DeviceId& id, const MotionSample& sample,
                                            std::uint64_t nowUs) {
    MotionSender snapshot;
    std::uint32_t deltaUs = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        MotionGate& gate = lastMotionUs_[id];
        if (gate.hasEmitted && nowUs - gate.lastUs < kMotionMinIntervalUs) {
            // Inside the rate-limit window — drop. Deliberately do NOT update
            // the gate; otherwise a hot stream of dropped samples would push
            // it forward and starve the legitimate sender for longer than
            // one period.
            return false;
        }
        if (gate.hasEmitted) {
            const std::uint64_t d = nowUs - gate.lastUs;
            deltaUs = (d > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<std::uint32_t>(d);
        }
        gate.lastUs = nowUs;
        gate.hasEmitted = true;
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
        auto it = lastBattery_.find(id);
        if (it != lastBattery_.end() && it->second == sample) {
            // Coalesced — no state transition since the last successful send.
            return;
        }
        lastBattery_[id] = sample;
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
    }
    if (snapshot) { snapshot(id, sample); }
}

std::int16_t scaleAxis(float v, float maxMagnitude) {
    const auto clamped = std::clamp(v, -1.0f, 1.0f);
    const auto scaled = static_cast<int>(clamped * maxMagnitude);
    return static_cast<std::int16_t>(
        std::clamp(scaled, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
}

std::uint8_t scaleTrigger(float v) {
    const auto clamped = std::clamp(v, 0.0f, 1.0f);
    const auto scaled = static_cast<int>(std::lround(clamped * 255.0f));
    return static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
}

GamepadInputProcessor::DeviceState applyDeadzones(const GamepadInputProcessor::DeviceState& state,
                                                  const GamepadInputProcessor::Deadzones& dz) {
    auto out = state;
    const auto stickFlat = static_cast<std::int32_t>(dz.stickFlat);
    if (std::abs(static_cast<std::int32_t>(out.lx)) <= stickFlat) { out.lx = 0; }
    if (std::abs(static_cast<std::int32_t>(out.ly)) <= stickFlat) { out.ly = 0; }
    if (std::abs(static_cast<std::int32_t>(out.rx)) <= stickFlat) { out.rx = 0; }
    if (std::abs(static_cast<std::int32_t>(out.ry)) <= stickFlat) { out.ry = 0; }
    if (out.lt <= dz.triggerFlat) { out.lt = 0; }
    if (out.rt <= dz.triggerFlat) { out.rt = 0; }
    return out;
}

} // namespace dish::input
