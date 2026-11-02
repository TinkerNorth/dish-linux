// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "GamepadInputProcessor.h"

#include "core/input/Deadzones.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace dish::input {

namespace {

// A read of a record its thread is writing is retried; the writer holds it for a few stores.
constexpr int kReadAttempts = 10'000;

std::uint32_t packDeadzones(const GamepadInputProcessor::Deadzones& dz) {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(dz.stickFlat)) |
           (static_cast<std::uint32_t>(dz.triggerFlat) << 16);
}

GamepadInputProcessor::Deadzones unpackDeadzones(std::uint32_t packed) {
    return {static_cast<std::int16_t>(static_cast<std::uint16_t>(packed & 0xFFFFU)),
            static_cast<std::uint8_t>((packed >> 16) & 0xFFU)};
}

template <typename Read> bool readWithRetry(Read&& read) {
    for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
        if (read()) { return true; }
        std::this_thread::yield();
    }
    return false;
}

} // namespace

void GamepadInputProcessor::setReportSender(ReportSender sender) { sender_ = std::move(sender); }

void GamepadInputProcessor::setMotionSender(MotionSender sender) {
    motionSender_ = std::move(sender);
}

void GamepadInputProcessor::setBatterySender(BatterySender sender) {
    batterySender_ = std::move(sender);
}

void GamepadInputProcessor::setTouchpadSender(TouchpadSender sender) {
    touchpadSender_ = std::move(sender);
}

void GamepadInputProcessor::setNanoTime(NanoTime nanoTime) { nanoTime_ = std::move(nanoTime); }

std::int64_t GamepadInputProcessor::steadyNanoTime() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool GamepadInputProcessor::idMatches(const Record& r, const DeviceId& id) const {
    IdText text;
    if (!readWithRetry([&] { return r.idSeq.read([&] { text = r.idText.load(); }); })) {
        return false;
    }
    return text.length == id.size() && std::memcmp(text.chars.data(), id.data(), id.size()) == 0;
}

std::optional<GamepadInputProcessor::DeviceIndex>
GamepadInputProcessor::find(const DeviceId& id) const {
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        if (r.state.load(std::memory_order_acquire) != kLive || !idMatches(r, id)) { continue; }
        if (r.state.load(std::memory_order_acquire) == kLive) {
            return static_cast<DeviceIndex>(i);
        }
    }
    return std::nullopt;
}

GamepadInputProcessor::Record* GamepadInputProcessor::recordAt(DeviceIndex index) {
    return index < records_.size() ? &records_[index] : nullptr;
}

// Two threads introducing the same id at once would both claim; the one holding the higher record
// waits for every lower claim to settle and gives its own back if one of them is the same id, so
// both end on the lower index. A claim never waits on a higher one, so the waits cannot cycle.
std::optional<GamepadInputProcessor::DeviceIndex> GamepadInputProcessor::add(const DeviceId& id) {
    if (id.size() > kMaxIdBytes) { return std::nullopt; }
    if (const auto known = find(id)) { return known; }
    for (std::size_t i = 0; i < records_.size(); ++i) {
        Record& r = records_[i];
        std::uint32_t free = kFree;
        if (!r.state.compare_exchange_strong(free, kClaimed, std::memory_order_acq_rel)) {
            continue;
        }
        IdText text;
        std::memcpy(text.chars.data(), id.data(), id.size());
        text.length = static_cast<std::uint8_t>(id.size());
        r.idSeq.beginWrite();
        r.idText.store(text);
        r.idSeq.endWrite();
        r.id = id;
        r.plugGen.fetch_add(1, std::memory_order_release);
        r.state.store(kLive, std::memory_order_release);
        for (std::size_t j = 0; j < i; ++j) {
            const Record& lower = records_[j];
            std::uint32_t state = lower.state.load(std::memory_order_acquire);
            while (state == kClaimed) {
                std::this_thread::yield();
                state = lower.state.load(std::memory_order_acquire);
            }
            if (state == kLive && idMatches(lower, id)) {
                r.state.store(kFree, std::memory_order_release);
                return static_cast<DeviceIndex>(j);
            }
        }
        return static_cast<DeviceIndex>(i);
    }
    return std::nullopt;
}

void GamepadInputProcessor::remove(const DeviceId& id) {
    const auto index = find(id);
    if (!index.has_value()) { return; }
    Record& r = records_[*index];
    r.seq.beginWrite();
    r.seen.store(0, std::memory_order_release);
    r.seq.endWrite();
    r.gamepadEvents.store(0, std::memory_order_relaxed);
    r.motionEvents.store(0, std::memory_order_relaxed);
    r.deadzones.store(0, std::memory_order_relaxed);
    r.plugGen.fetch_add(1, std::memory_order_release);
    r.state.store(kFree, std::memory_order_release);
}

void GamepadInputProcessor::setDeadzones(const DeviceId& id, const Deadzones& dz) {
    if (const auto index = add(id)) {
        records_[*index].deadzones.store(packDeadzones(dz), std::memory_order_relaxed);
    }
}

void GamepadInputProcessor::syncOwner(Record& r) {
    if (const std::uint32_t gen = r.plugGen.load(std::memory_order_acquire); gen != r.ownGen) {
        r.ownGen = gen;
        r.forwarded = false;
        r.lastForwarded = DeviceState{};
        r.activityRef = DeviceState{};
        r.gate = MotionGate{};
        r.lastTouchpad.reset();
    }
    if (const std::uint32_t gen = r.zeroGen.load(std::memory_order_acquire); gen != r.ownZeroGen) {
        r.ownZeroGen = gen;
        r.forwarded = true;
        r.lastForwarded = DeviceState{};
    }
}

void GamepadInputProcessor::publish(DeviceIndex index, const DeviceState& state) {
    Record* const r = recordAt(index);
    if (r == nullptr) { return; }
    syncOwner(*r);
    const DeviceState filtered =
        applyDeadzones(state, unpackDeadzones(r->deadzones.load(std::memory_order_relaxed)));
    r->seq.beginWrite();
    r->raw.store(state);
    r->wire.store(filtered);
    r->seen.store(r->seen.load(std::memory_order_relaxed) | kSeenReport, std::memory_order_release);
    r->seq.endWrite();
    if (isActuation(r->activityRef, filtered)) {
        r->activityRef = filtered;
        actuations_.fetch_add(1, std::memory_order_relaxed);
    }
    r->gamepadEvents.fetch_add(1, std::memory_order_relaxed);
    telEvents_.fetch_add(1, std::memory_order_relaxed);
    if (r->forwarded && r->lastForwarded == filtered) { return; }
    r->forwarded = true;
    r->lastForwarded = filtered;
    telSends_.fetch_add(1, std::memory_order_relaxed);
    telTotalSent_.fetch_add(1, std::memory_order_relaxed);
    if (sink_ != nullptr) { sink_->report(index, r->id, filtered); }
    if (sender_) {
        sender_(r->id, filtered.wButtons, filtered.lt, filtered.rt, filtered.lx, filtered.ly,
                filtered.rx, filtered.ry);
    }
}

bool GamepadInputProcessor::publishMotionAt(DeviceIndex index, const MotionSample& sample,
                                            std::uint64_t nowUs) {
    Record* const r = recordAt(index);
    if (r == nullptr) { return false; }
    syncOwner(*r);
    MotionGate& gate = r->gate;
    if (gate.hasEmitted && nowUs - gate.lastUs < kMotionMinIntervalUs) {
        // Deliberately do NOT advance `gate`: a hot stream of dropped
        // samples would push it forward and starve the sender for longer
        // than one period.
        return false;
    }
    // Relative to the previous *emitted* packet, not the previous attempt,
    // so the receiver gets true inter-arrival timing. Delta stays 0 on the
    // first emission — the wire spec's first-packet sentinel.
    std::uint32_t deltaUs = 0;
    if (gate.hasEmitted) {
        const std::uint64_t d = nowUs - gate.lastUs;
        deltaUs = (d > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<std::uint32_t>(d);
    }
    gate.lastUs = nowUs;
    gate.hasEmitted = true;
    r->seq.beginWrite();
    r->motion.store(sample);
    r->seen.store(r->seen.load(std::memory_order_relaxed) | kSeenMotion, std::memory_order_release);
    r->seq.endWrite();
    r->motionEvents.fetch_add(1, std::memory_order_relaxed);
    if (sink_ != nullptr) { sink_->motion(index, r->id, sample, deltaUs); }
    if (motionSender_) {
        motionSender_(r->id, sample.gyroX, sample.gyroY, sample.gyroZ, sample.accelX, sample.accelY,
                      sample.accelZ, deltaUs);
    }
    return true;
}

bool GamepadInputProcessor::publishMotion(DeviceIndex index, const MotionSample& sample) {
    const auto now = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    return publishMotionAt(index, sample, us);
}

void GamepadInputProcessor::publishBattery(DeviceIndex index, const BatterySample& sample) {
    Record* const r = recordAt(index);
    if (r == nullptr) { return; }
    if (sink_ != nullptr) { sink_->battery(index, r->id, sample); }
    if (batterySender_) { batterySender_(r->id, sample.level, sample.status); }
}

void GamepadInputProcessor::publishTouchpad(DeviceIndex index, const TouchpadSample& sample) {
    Record* const r = recordAt(index);
    if (r == nullptr) { return; }
    syncOwner(*r);
    r->seq.beginWrite();
    r->touchpad.store(sample);
    r->seen.store(r->seen.load(std::memory_order_relaxed) | kSeenTouchpad,
                  std::memory_order_release);
    r->seq.endWrite();
    const bool touching = sample.finger0Active || sample.finger1Active || sample.buttonPressed;
    const bool changed = r->lastTouchpad.has_value() && !sample.sameTouch(*r->lastTouchpad);
    r->lastTouchpad = sample;
    if (touching || changed) { actuations_.fetch_add(1, std::memory_order_relaxed); }
    if (sink_ != nullptr) { sink_->touchpad(index, r->id, sample); }
    if (touchpadSender_) { touchpadSender_(r->id, sample); }
}

void GamepadInputProcessor::publish(const DeviceId& id, const DeviceState& state) {
    if (const auto index = add(id)) { publish(*index, state); }
}

bool GamepadInputProcessor::publishMotion(const DeviceId& id, const MotionSample& sample) {
    const auto index = add(id);
    return index.has_value() && publishMotion(*index, sample);
}

bool GamepadInputProcessor::publishMotionAt(const DeviceId& id, const MotionSample& sample,
                                            std::uint64_t nowUs) {
    const auto index = add(id);
    return index.has_value() && publishMotionAt(*index, sample, nowUs);
}

void GamepadInputProcessor::publishBattery(const DeviceId& id, const BatterySample& sample) {
    if (const auto index = add(id)) { publishBattery(*index, sample); }
}

void GamepadInputProcessor::publishTouchpad(const DeviceId& id, const TouchpadSample& sample) {
    if (const auto index = add(id)) { publishTouchpad(*index, sample); }
}

void GamepadInputProcessor::zeroAndSendAll() {
    for (Record& r : records_) {
        if (r.state.load(std::memory_order_acquire) != kLive) { continue; }
        IdText text;
        if (!readWithRetry([&] { return r.idSeq.read([&] { text = r.idText.load(); }); })) {
            continue;
        }
        r.zeroGen.fetch_add(1, std::memory_order_release);
        if (sender_) { sender_(DeviceId(text.chars.data(), text.length), 0, 0, 0, 0, 0, 0, 0); }
    }
}

GamepadInputProcessor::Inspection GamepadInputProcessor::inspect(const DeviceId& id) const {
    Inspection seen;
    const auto index = find(id);
    if (!index.has_value()) { return seen; }
    const Record& r = records_[*index];
    DeviceState wire;
    DeviceState raw;
    MotionSample motion;
    TouchpadSample touchpad;
    std::uint32_t bits = 0;
    const bool read = readWithRetry([&] {
        return r.seq.read([&] {
            wire = r.wire.load();
            raw = r.raw.load();
            motion = r.motion.load();
            touchpad = r.touchpad.load();
            bits = r.seen.load(std::memory_order_acquire);
        });
    });
    if (!read) { return seen; }
    if ((bits & kSeenReport) != 0) {
        seen.wire = wire;
        seen.raw = raw;
    }
    if ((bits & kSeenMotion) != 0) { seen.motion = motion; }
    if ((bits & kSeenTouchpad) != 0) { seen.touchpad = touchpad; }
    return seen;
}

GamepadInputProcessor::InputRateCounters
GamepadInputProcessor::inputCounters(const DeviceId& id) const {
    const auto index = find(id);
    if (!index.has_value()) { return InputRateCounters{}; }
    const Record& r = records_[*index];
    return InputRateCounters{r.gamepadEvents.load(std::memory_order_relaxed),
                             r.motionEvents.load(std::memory_order_relaxed)};
}

GamepadInputProcessor::TelemetrySnapshot GamepadInputProcessor::drainTelemetry() {
    return TelemetrySnapshot{telEvents_.exchange(0, std::memory_order_relaxed),
                             telSends_.exchange(0, std::memory_order_relaxed),
                             telTotalSent_.load(std::memory_order_relaxed)};
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
