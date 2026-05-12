// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "GamepadInputProcessor.h"

#include <algorithm>
#include <cmath>

namespace dish::input {

void GamepadInputProcessor::setReportSender(ReportSender sender) {
    std::lock_guard<std::mutex> lock(mtx_);
    sender_ = std::move(sender);
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
        ++telEvents_;
        ++telSends_;
        ++telTotalSent_;
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
}

GamepadInputProcessor::TelemetrySnapshot GamepadInputProcessor::drainTelemetry() {
    std::lock_guard<std::mutex> lock(mtx_);
    TelemetrySnapshot snap{telEvents_, telSends_, telTotalSent_};
    telEvents_ = 0;
    telSends_ = 0;
    return snap;
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
