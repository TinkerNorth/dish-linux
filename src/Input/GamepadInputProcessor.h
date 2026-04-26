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

    struct TelemetrySnapshot {
        int events = 0;
        int sends = 0;
        std::uint64_t totalSent = 0;
    };

    void setReportSender(ReportSender sender);
    void publish(const DeviceId& id, const DeviceState& state);
    void zeroAndSendAll();
    void remove(const DeviceId& id);
    TelemetrySnapshot drainTelemetry();

  private:
    std::mutex mtx_;
    std::unordered_map<DeviceId, DeviceState> states_;
    ReportSender sender_;
    int telEvents_ = 0;
    int telSends_ = 0;
    std::uint64_t telTotalSent_ = 0;
};

// Pure helpers — easily testable.
std::int16_t scaleAxis(float v, float maxMagnitude);
std::uint8_t scaleTrigger(float v);

} // namespace dish::input
