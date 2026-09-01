// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The unit translation between the satellite's wire conventions and Moonlight's,
// plus the host's motion subscription gate. One owner for both, because the two
// transports share every telemetry source: the same decoded pad report feeds a
// satellite MSG_MOTION and a Moonlight CONTROLLER_MOTION, and only the units and
// the cadence differ.
//
// Satellite (docs/contract.md): fixed-point int16, gyro at +/-2000 deg/s full
// scale, accel at +/-4 g, touch coordinates full-range int16.
// Moonlight (Wolf control.hpp): IEEE-754 floats, gyro in deg/s, accel in
// METRES PER SECOND SQUARED, touch normalised 0..1 across the pad.
//
// Forwarding the raw int16 as a float is not a rounding error, it is a
// different quantity: a gyro at rest reads 0 either way, so the mistake only
// shows up as a host seeing a pad spinning at 30000 deg/s the moment it moves.

#pragma once

#include "core/moonlight/MoonlightProtocol.h"

#include <cstdint>
#include <map>
#include <mutex>

namespace dish::moonlight {

// Satellite int16 -> deg/s. Same unit both ends, so this is pure scale.
inline constexpr float kGyroScaleDegS = 2000.0F / 32767.0F;
// Satellite int16 -> g.
inline constexpr float kAccelScaleG = 4.0F / 32767.0F;
// ...and Moonlight wants m/s^2, so the g value is multiplied through. Leaving
// this out is a silent 9.8x under-report that a host reads as a pad lying flat
// however hard it is shaken.
inline constexpr float kStandardGravity = 9.80665F;

inline float gyroDegS(std::int16_t wire) { return static_cast<float>(wire) * kGyroScaleDegS; }

inline float accelMs2(std::int16_t wire) {
    return static_cast<float>(wire) * kAccelScaleG * kStandardGravity;
}

// Full-range int16 -> 0..1. The offset makes the minimum exactly 0 and the
// maximum exactly 1, so a finger at the pad's edge does not land at 0.5.
inline float touchNorm(std::int16_t wire) {
    return static_cast<float>(static_cast<std::int32_t>(wire) + 32768) / 65535.0F;
}

// Satellite battery status (0 unknown, 1 discharging, 2 charging, 3 full,
// 4 wired) -> Moonlight BATTERY_STATE, so the one battery publisher feeds both
// transports.
inline std::uint8_t batteryStateFromSatelliteStatus(std::uint8_t satelliteStatus) {
    switch (satelliteStatus) {
    case 1:
        return moonproto::kBatteryDischarging;
    case 2:
        return moonproto::kBatteryCharging;
    case 3:
        return moonproto::kBatteryFull;
    case 4:
        // Wired with no charge telemetry reads as charging, the closest state
        // the wire has. NOT_PRESENT would tell the host there is no battery,
        // which is a different and usually wrong claim about the hardware.
        return moonproto::kBatteryCharging;
    default:
        return moonproto::kBatteryStateUnknown;
    }
}

// Satellite level (0..100, or 0xFF for unknown) -> Moonlight percentage. Any
// out-of-range value is reported as unknown rather than clamped: a clamp would
// show the user a confident 100% for a pad that never answered.
inline std::uint8_t batteryPercentage(std::uint8_t level) {
    return level <= 100 ? level : moonproto::kBatteryPercentageUnknown;
}

// Host-requested motion streaming for one session (MOTION_EVENT 0x5501): per
// (controller number, motion type) the requested report rate, 0 = stop.
//
// The default is OFF. A host that never subscribes gets no motion at all, which
// is the point: an unasked-for IMU stream is bandwidth the host throws away, and
// Sunshine only asks once a game has opened the sensor.
//
// Per (pad, type), not per session: one session drives up to four pads and a
// host subscribes to each independently, so a single session-wide flag would
// start every pad's stream the moment one game opened one sensor.
//
// Senders keep their own cadence -- a pad reports at whatever rate its hardware
// polls -- so shouldSend() applies the host's ceiling on top, dropping samples
// that arrive faster than asked for rather than trying to reschedule them.
//
// Thread-safe: the control-stream receive thread writes subscriptions while the
// input thread reads them.
class MoonlightMotionGate {
  public:
    // rateHz 0 (or negative) unsubscribes, which is how a host says stop.
    void onMotionRequest(int controllerNumber, int reportRateHz, int motionType) {
        const Key key{controllerNumber, motionType};
        std::lock_guard<std::mutex> lock(mtx_);
        if (reportRateHz <= 0) {
            rates_.erase(key);
            lastSentUs_.erase(key);
            return;
        }
        rates_[key] = reportRateHz;
    }

    // Forget one pad's subscriptions (it unbound), or all of them (the session
    // ended). A returning pad must wait to be asked again rather than resuming
    // a stream the host has forgotten about.
    void clear(int controllerNumber) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = rates_.begin(); it != rates_.end();) {
            it = it->first.controllerNumber == controllerNumber ? rates_.erase(it) : std::next(it);
        }
        for (auto it = lastSentUs_.begin(); it != lastSentUs_.end();) {
            it = it->first.controllerNumber == controllerNumber ? lastSentUs_.erase(it)
                                                                : std::next(it);
        }
    }

    void clearAll() {
        std::lock_guard<std::mutex> lock(mtx_);
        rates_.clear();
        lastSentUs_.clear();
    }

    bool wanted(int controllerNumber, int motionType) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return rates_.count(Key{controllerNumber, motionType}) != 0;
    }

    // True, and marks the send, when a sample of this type is due under the
    // requested rate. `nowUs` is any monotonic microsecond clock.
    bool shouldSend(int controllerNumber, int motionType, std::int64_t nowUs) {
        const Key key{controllerNumber, motionType};
        std::lock_guard<std::mutex> lock(mtx_);
        const auto rate = rates_.find(key);
        if (rate == rates_.end()) { return false; }
        const std::int64_t intervalUs = 1000000 / rate->second;
        const auto last = lastSentUs_.find(key);
        if (last != lastSentUs_.end() && nowUs - last->second < intervalUs) { return false; }
        lastSentUs_[key] = nowUs;
        return true;
    }

  private:
    struct Key {
        int controllerNumber;
        int motionType;
        bool operator<(const Key& o) const {
            return controllerNumber != o.controllerNumber ? controllerNumber < o.controllerNumber
                                                          : motionType < o.motionType;
        }
    };

    mutable std::mutex mtx_;
    std::map<Key, int> rates_;
    std::map<Key, std::int64_t> lastSentUs_;
};

} // namespace dish::moonlight
