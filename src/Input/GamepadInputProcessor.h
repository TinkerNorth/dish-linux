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
// server expects. Mirrors dish-mac/Input/GamepadInputProcessor.swift and
// dish-android/GamepadInputProcessor.kt. Pure logic; no Qt or SDL dependency.
class GamepadInputProcessor {
  public:
    // XUSB button bits (identical to Android BUTTON_MAP).
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

    // Invoked every time a report is emitted. Called on the caller's thread
    // (typically the SDL gamepad thread) for lowest latency.
    using ReportSender = std::function<void(const DeviceId& id, std::uint16_t wButtons,
                                            std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                                            std::int16_t ly, std::int16_t rx, std::int16_t ry)>;

    // Single IMU sample destined for MSG_MOTION. The processor rate-limits
    // these per-device to `kMotionRateLimitHz` (default 250) before invoking
    // the sender — matches the roadmap acceptance criterion that motion
    // packets stay under 250 Hz by default.
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

    // Periodic battery sample destined for MSG_BATTERY. Forwarded as-is:
    // MSG_BATTERY is a fixed 30 s heartbeat (plus on-connect), so the
    // receiver expects a packet every interval even when the value is
    // unchanged — that is what lets a dropped UDP packet self-heal. The
    // SDL bridge's per-device 30 s poll gate owns the cadence.
    struct BatterySample {
        std::uint8_t level = 0xFF;
        std::uint8_t status = 0;
    };
    using BatterySender =
        std::function<void(const DeviceId& id, std::uint8_t level, std::uint8_t status)>;

    // Touchpad sample destined for MSG_TOUCHPAD (DualSense / DS4). Up to two
    // fingers; coordinates are normalised int16. Touchpad input is genuinely
    // event-driven (finger down/move/up), so unlike motion it is neither
    // rate-limited nor coalesced — every assembled state change is forwarded.
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

    // Maximum forwarded MSG_MOTION rate per controller. Roadmap acceptance:
    // packets are rate-limited to ≤ 250 Hz by default. Samples arriving
    // faster than this drop on the floor; the next within-budget sample
    // is sent.
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

    // Per-axis deadzone thresholds. Values whose absolute magnitude is at or
    // below the flat are zeroed before the report leaves the processor —
    // mirrors the per-device `flat` values Android pulls out of
    // `InputDevice.getMotionRange(axis).getFlat()`. SDL2 doesn't surface an
    // OS-level equivalent, so SDLGamepadBridge installs a sensible default
    // when each device attaches.
    struct Deadzones {
        std::int16_t stickFlat = 0;
        std::uint8_t triggerFlat = 0;
        bool operator==(const Deadzones& o) const {
            return stickFlat == o.stickFlat && triggerFlat == o.triggerFlat;
        }
    };

    void setReportSender(ReportSender sender);
    void setMotionSender(MotionSender sender);
    void setBatterySender(BatterySender sender);
    void setTouchpadSender(TouchpadSender sender);
    void setDeadzones(const DeviceId& id, const Deadzones& dz);
    void publish(const DeviceId& id, const DeviceState& state);

    // Publish an IMU sample for `id`. Per-device rate-limited to
    // kMotionRateLimitHz; samples inside the gate window drop silently.
    void publishMotion(const DeviceId& id, const MotionSample& sample);

    // Test seam — accepts a caller-supplied "now" timestamp in microseconds
    // (any monotonic basis). Returns whether the sample was forwarded.
    bool publishMotionAt(const DeviceId& id, const MotionSample& sample, std::uint64_t nowUs);

    // Publish a battery sample for `id`. Pure pass-through to the
    // BatterySender — every poll tick is forwarded, unchanged or not, so the
    // 30 s heartbeat actually reaches the wire. Cadence is owned by the SDL
    // bridge's poll gate, not this method.
    void publishBattery(const DeviceId& id, const BatterySample& sample);

    // Publish a touchpad sample for `id`. Pure pass-through to the
    // TouchpadSender — the SDL bridge has already assembled the full
    // two-finger state, and a touchpad is an absolute surface so there is no
    // deadzone / rate-limit / coalesce step.
    void publishTouchpad(const DeviceId& id, const TouchpadSample& sample);

    void zeroAndSendAll();
    void remove(const DeviceId& id);

  private:
    std::mutex mtx_;
    std::unordered_map<DeviceId, DeviceState> states_;
    std::unordered_map<DeviceId, Deadzones> deadzones_;
    ReportSender sender_;
    MotionSender motionSender_;
    BatterySender batterySender_;
    TouchpadSender touchpadSender_;

    // Per-device motion rate-limit gate. `lastUs` is the timestamp of the
    // last *emitted* sample (microseconds, steady_clock basis or
    // test-supplied). `hasEmitted` is an explicit "have we ever emitted"
    // flag rather than overloading `lastUs == 0` as the sentinel — a
    // monotonic or test clock can legitimately read 0, which would
    // otherwise misread the second sample as another first sample.
    struct MotionGate {
        std::uint64_t lastUs = 0;
        bool hasEmitted = false;
    };
    std::unordered_map<DeviceId, MotionGate> lastMotionUs_;
};

// Pure helpers — easily testable.
std::int16_t scaleAxis(float v, float maxMagnitude);
std::uint8_t scaleTrigger(float v);

// Pure deadzone application. Sticks: `|v| <= flat → 0`. Triggers: `v <= flat
// → 0`. Buttons are passed through. Extracted as a free function so tests can
// pin the arithmetic without the processor's lock plumbing.
GamepadInputProcessor::DeviceState applyDeadzones(const GamepadInputProcessor::DeviceState& state,
                                                  const GamepadInputProcessor::Deadzones& dz);

} // namespace dish::input
