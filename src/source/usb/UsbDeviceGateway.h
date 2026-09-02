// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The IO boundary the USB-direct claim driver talks to. The seam exists so the
// FSM and the driver can be tested against a fake device with no real USB or HID
// IO, while production swaps in HidrawGateway.
//
// IO only, no domain state beyond an in-flight claim. The decision of when to
// claim, release or bind belongs to the pure reducer.

#pragma once

#include "core/reducer/DirectClaimFailure.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dish::source::usb {

// A gamepad-shaped USB device the gateway found (one interrupt-IN endpoint on a
// HID or vendor-specific interface). Mirrors the fields android reads off
// UsbDevice + the chosen interrupt-IN endpoint.
struct UsbDeviceInfo {
    int vendorId = 0;
    int productId = 0;
    std::string name;             // friendly/product name (or the device path).
    int interfaceNumber = 0;      // the claimed interface's number.
    int endpointInMaxPacket = 64; // interrupt-IN wMaxPacketSize.
    int endpointInInterval = 1;   // interrupt-IN bInterval.
    bool hasOutEndpoint = false;  // an interrupt-OUT endpoint exists (for rumble).
    bool hasImu = false;          // the model carries a gyro/accel (DualSense/DS4).

    int vpKey() const { return (vendorId << 16) | (productId & 0xFFFF); }
};

// The result of opening + claiming a device's interface and bringing up its
// read loop. On success a synthetic device id (negative, like android's) is
// handed back; on failure the cause + whether the framework interface was
// stolen (so the FSM knows if it must wait for re-enumeration).
struct ClaimResult {
    bool ok = false;
    int syntheticId = 0;                                                    // valid iff ok.
    reducer::DirectClaimFailure reason = reducer::DirectClaimFailure::Busy; // valid iff !ok.
    bool frameworkStolen = false;                                           // valid iff !ok.

    static ClaimResult success(int syntheticId) {
        return ClaimResult{true, syntheticId, {}, false};
    }
    static ClaimResult fail(reducer::DirectClaimFailure reason, bool frameworkStolen) {
        return ClaimResult{false, 0, reason, frameworkStolen};
    }
};

// A decoded controller report from the claimed device's read loop, already
// normalised to the XUSB axis/trigger scale (the gateway owns the per-model HID
// report parse via core/input/UsbReportParsers). `wButtons` is the XUSB button
// word the decoders emit directly (GamepadButtonLayouts kXusb* bits) — the driver
// publishes it straight to GamepadInputProcessor with NO further conversion, so
// the decode->publish path stays allocation-free. The IMU + touchpad fields carry
// the in-band motion / touchpad surface for the pads that have them (DS4 /
// DualSense / Switch Pro); `motionValid` / `touchpadValid` gate whether the
// manager forwards a MOTION / TOUCHPAD packet for this report.
struct UsbReport {
    std::uint16_t wButtons = 0; // XUSB button bits (already mapped by the decoder).
    std::uint8_t lt = 0;
    std::uint8_t rt = 0;
    std::int16_t lx = 0;
    std::int16_t ly = 0;
    std::int16_t rx = 0;
    std::int16_t ry = 0;

    // IMU (wire int16 scale). Forwarded as MSG_MOTION only when motionValid.
    bool motionValid = false;
    std::int16_t gyroX = 0;
    std::int16_t gyroY = 0;
    std::int16_t gyroZ = 0;
    std::int16_t accelX = 0;
    std::int16_t accelY = 0;
    std::int16_t accelZ = 0;

    // Touchpad (DS4 / DualSense). Forwarded as MSG_TOUCHPAD only when touchpadValid.
    bool touchpadValid = false;
    bool finger0Active = false;
    std::uint8_t finger0Id = 0;
    std::int16_t finger0X = 0;
    std::int16_t finger0Y = 0;
    bool finger1Active = false;
    std::uint8_t finger1Id = 0;
    std::int16_t finger1X = 0;
    std::int16_t finger1Y = 0;
    bool touchpadButton = false;

    // The decoder's mic-mute LATCH state (DualSense only; false elsewhere).
    // Already folded into wButtons as kXusbMicMute by the decoder — this field
    // exists so the driver can mirror the state up as an edge without
    // re-reading button bits.
    bool micMuted = false;
};

class UsbDeviceGateway {
  public:
    virtual ~UsbDeviceGateway() = default;

    // Enumerate the gamepad-shaped USB/HID devices present right now. Empty if
    // none. Xbox-class pads are NOT returned: xpad publishes no hidraw node for
    // them, so they stay on the SDL path.
    virtual std::vector<UsbDeviceInfo> enumerate() = 0;

    // Open + claim the device's interrupt interface and start its read loop,
    // returning a ClaimResult. `onReport` is invoked from the gateway's read
    // thread for each decoded report — plain C++, no allocation per call on the
    // hot path. The synthetic id in a successful result is what the driver binds
    // and later passes to releaseClaim/markTransitioning.
    virtual ClaimResult claim(const UsbDeviceInfo& device,
                              std::function<void(const UsbReport&)> onReport) = 0;

    // Stop the read loop and release the interface for a claimed synthetic id.
    // Idempotent; a no-op for an unknown id.
    virtual void releaseClaim(int syntheticId) = 0;

    // Whether the model is a verified "fast-lane" pad worth auto-claiming Direct
    // (DualSense/DS4/Switch Pro/8BitDo families).
    virtual bool isKnownFastLaneModel(int vendorId, int productId) const = 0;

    // The model's current measured completion count (URB/transfer count) for the
    // poll-rate sampler. 0 if unknown / not claimed.
    virtual std::int64_t completionCount(int syntheticId) const = 0;

    // Write one OUT report to a claimed device: rumble, a lightbar colour,
    // player LEDs, trigger effects. `data[0]` is the report id and the buffer is
    // the whole report exactly as core/input/UsbOutputReports.h built it; the
    // gateway adds only framing the platform itself demands and never reasons
    // about the contents. Returns false for an unknown id, a device with no OUT
    // path, or a failed write.
    //
    // Callable from any thread and from inside a report callback: feedback
    // arrives on the network receive thread while the read loop is mid-transfer.
    virtual bool writeOutputReport(int syntheticId, const std::uint8_t* data, std::size_t len) = 0;

    // Overwrite the claimed device's mic-mute latch (the state the DualSense
    // decoder folds onto the wire), so the app's mute control and the pad's
    // own button stay one state. False for an unknown id or a family with no
    // latch. Default no-op so fakes that never touch audio need no override.
    virtual bool setPadMicMuted(int syntheticId, bool muted) {
        (void)syntheticId;
        (void)muted;
        return false;
    }
};

} // namespace dish::source::usb
