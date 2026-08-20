// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace dish::input {

// Converts raw gamepad values into the XUSB report format the Satellite
// server expects. Pure logic; no SDL dependency.
class GamepadInputProcessor {
  public:
    // Wire values — do not renumber.
    struct Buttons {
        static constexpr std::uint16_t kDpadUp = 0x0001;
        static constexpr std::uint16_t kDpadDown = 0x0002;
        static constexpr std::uint16_t kDpadLeft = 0x0004;
        static constexpr std::uint16_t kDpadRight = 0x0008;
        static constexpr std::uint16_t kStart = 0x0010;
        static constexpr std::uint16_t kBack = 0x0020;
        static constexpr std::uint16_t kLeftThumb = 0x0040;
        static constexpr std::uint16_t kRightThumb = 0x0080;
        static constexpr std::uint16_t kLeftShoulder = 0x0100;
        static constexpr std::uint16_t kRightShoulder = 0x0200;
        static constexpr std::uint16_t kA = 0x1000;
        static constexpr std::uint16_t kB = 0x2000;
        static constexpr std::uint16_t kX = 0x4000;
        static constexpr std::uint16_t kY = 0x8000;
    };

    using DeviceId = std::string;

    // Runs inline on the caller's thread (normally the SDL gamepad thread);
    // the hot path deliberately never hops threads.
    using ReportSender = std::function<void(const DeviceId& id, std::uint16_t wButtons,
                                            std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                                            std::int16_t ly, std::int16_t rx, std::int16_t ry)>;

    // Rate-limited per device to kMotionRateLimitHz before it reaches the
    // sender.
    struct MotionSample {
        std::int16_t gyroX = 0;
        std::int16_t gyroY = 0;
        std::int16_t gyroZ = 0;
        std::int16_t accelX = 0;
        std::int16_t accelY = 0;
        std::int16_t accelZ = 0;
    };
    using MotionSender =
        std::function<void(const DeviceId& id, std::int16_t gyroX, std::int16_t gyroY,
                           std::int16_t gyroZ, std::int16_t accelX, std::int16_t accelY,
                           std::int16_t accelZ, std::uint32_t timestampDeltaUs)>;

    // Never coalesced: MSG_BATTERY is a fixed 30 s heartbeat, so an unchanged
    // value must still reach the wire for a dropped packet to self-heal. The
    // SDL bridge's poll gate owns the cadence.
    struct BatterySample {
        std::uint8_t level = 0xFF;
        std::uint8_t status = 0;
    };
    using BatterySender =
        std::function<void(const DeviceId& id, std::uint8_t level, std::uint8_t status)>;

    // Genuinely event-driven (finger down/move/up), so unlike motion it is
    // neither rate-limited nor coalesced.
    struct TouchpadSample {
        bool finger0Active = false;
        std::uint8_t finger0Id = 0;
        std::int16_t finger0X = 0;
        std::int16_t finger0Y = 0;
        bool finger1Active = false;
        std::uint8_t finger1Id = 0;
        std::int16_t finger1X = 0;
        std::int16_t finger1Y = 0;
        bool buttonPressed = false;
    };
    using TouchpadSender = std::function<void(const DeviceId& id, const TouchpadSample& sample)>;

    // Max forwarded MSG_MOTION rate per controller; faster samples are dropped.
    static constexpr std::uint32_t kMotionRateLimitHz = 250;
    static constexpr std::uint64_t kMotionMinIntervalUs =
        1'000'000ULL / kMotionRateLimitHz; // 4000 µs

    struct DeviceState {
        std::uint16_t wButtons = 0;
        std::uint8_t lt = 0;
        std::uint8_t rt = 0;
        std::int16_t lx = 0;
        std::int16_t ly = 0;
        std::int16_t rx = 0;
        std::int16_t ry = 0;
        bool operator==(const DeviceState& o) const {
            return wButtons == o.wButtons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly &&
                   rx == o.rx && ry == o.ry;
        }
    };

    // Values at or below the flat are zeroed before the report leaves the
    // processor. SDL2 exposes no OS-level per-device flat, so SDLGamepadBridge
    // installs a default on attach.
    struct Deadzones {
        std::int16_t stickFlat = 0;
        std::uint8_t triggerFlat = 0;
        bool operator==(const Deadzones& o) const {
            return stickFlat == o.stickFlat && triggerFlat == o.triggerFlat;
        }
    };

    struct TelemetrySnapshot {
        int events = 0;
        int sends = 0;
        std::uint64_t totalSent = 0;
    };

    // The seam InputRateStore samples to derive Hz. `motion` counts *forwarded*
    // samples, not attempts, or a throttled stream would over-report. Monotonic
    // within a device's lifetime; remove() drops the entry so a re-attach
    // re-baselines instead of appearing to leap forward.
    struct InputRateCounters {
        std::uint64_t gamepadEvents = 0;
        std::uint64_t motionEvents = 0;
    };

    // 0/0 if the device never emitted. Off the hot path (~1 Hz, Qt main thread)
    // and takes the existing mtx_, so the send path gains no new lock.
    InputRateCounters inputCounters(const DeviceId& id) const;

    void setReportSender(ReportSender sender);
    void setMotionSender(MotionSender sender);
    void setBatterySender(BatterySender sender);
    void setTouchpadSender(TouchpadSender sender);
    void setDeadzones(const DeviceId& id, const Deadzones& dz);
    void publish(const DeviceId& id, const DeviceState& state);

    // Samples inside the rate-limit window drop silently.
    void publishMotion(const DeviceId& id, const MotionSample& sample);

    // Test seam taking an explicit `now` (microseconds, any monotonic basis).
    // Returns false when the rate limiter dropped the sample.
    bool publishMotionAt(const DeviceId& id, const MotionSample& sample, std::uint64_t nowUs);

    void publishBattery(const DeviceId& id, const BatterySample& sample);
    void publishTouchpad(const DeviceId& id, const TouchpadSample& sample);

    void zeroAndSendAll();
    void remove(const DeviceId& id);
    TelemetrySnapshot drainTelemetry();

  private:
    mutable std::mutex mtx_;
    std::unordered_map<DeviceId, DeviceState> states_;
    std::unordered_map<DeviceId, Deadzones> deadzones_;
    ReportSender sender_;
    MotionSender motionSender_;
    BatterySender batterySender_;
    TouchpadSender touchpadSender_;

    // `lastUs` is the last *emitted* sample. `hasEmitted` is a separate flag,
    // not a `lastUs == 0` sentinel: a monotonic clock can legitimately read 0,
    // which would make the second sample look like another first sample.
    struct MotionGate {
        std::uint64_t lastUs = 0;
        bool hasEmitted = false;
    };
    std::unordered_map<DeviceId, MotionGate> lastMotionUs_;

    // Bumped inside the mtx_ section publish()/publishMotionAt() already hold,
    // so the hot path gains no new lock and no per-event allocation (the map
    // node is made on a device's first event, then reused). std::atomic is
    // neither copyable nor movable; unordered_map::operator[] default-
    // constructs in place, so the nodes are never moved.
    struct AtomicInputCounters {
        std::atomic<std::uint64_t> gamepad{0};
        std::atomic<std::uint64_t> motion{0};
    };
    std::unordered_map<DeviceId, AtomicInputCounters> rateCounters_;

    int telEvents_ = 0;
    int telSends_ = 0;
    std::uint64_t telTotalSent_ = 0;
};

std::int16_t scaleAxis(float v, float maxMagnitude);
std::uint8_t scaleTrigger(float v);

// Free function so tests can pin the arithmetic without the lock plumbing.
GamepadInputProcessor::DeviceState applyDeadzones(const GamepadInputProcessor::DeviceState& state,
                                                  const GamepadInputProcessor::Deadzones& dz);

} // namespace dish::input
