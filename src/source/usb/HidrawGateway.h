// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Linux hidraw implementation of UsbDeviceGateway: the USB-direct claim
// path for HID gamepads.
//
// Unlike Windows, hidraw hands back the raw report descriptor (HIDIOCGRDESC),
// so generic pads decode through the canonical core/input/UsbHidLayout parser
// rather than a preparsed-data approximation of it. Xbox-class pads are still
// out of reach here, but for a different reason than on Windows: xpad binds
// them as evdev-only and publishes no hidraw node, so they stay on the SDL
// path.
//
// Claiming requires read/write on /dev/hidraw*, which is root-only by default.
// packaging/udev/70-dish-hidraw.rules grants the `input` group access to the
// supported models; without it every claim fails PermissionDenied and the FSM
// keeps the pad on SDL.

#pragma once

#include "source/usb/UsbDeviceGateway.h"

#include "core/input/UsbHidLayout.h"
#include "core/input/UsbReportParsers.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dish::source::usb {

class HidrawGateway : public UsbDeviceGateway {
  public:
    HidrawGateway();
    ~HidrawGateway() override;

    std::vector<UsbDeviceInfo> enumerate() override;
    ClaimResult claim(const UsbDeviceInfo& device,
                      std::function<void(const UsbReport&)> onReport) override;
    void releaseClaim(int syntheticId) override;
    bool isKnownFastLaneModel(int vendorId, int productId) const override;
    std::int64_t completionCount(int syntheticId) const override;

  private:
    struct Claimed {
        std::string node;
        int fd = -1;
        std::thread reader;
        std::atomic<bool> running{false};
        std::atomic<std::int64_t> completions{0};
        std::function<void(const UsbReport&)> onReport;
        int vendorId = 0;
        int productId = 0;
        // Reader-thread-only: the reader is sole owner once started and is
        // joined before this is destroyed, so neither needs a lock.
        input::usbparse::HidParser parser = input::usbparse::HidParser::None;
        input::usbparse::StickAutoRangeState sticks;
        input::usbhid::HidLayout layout;
        int featureReportLen = 0;

        ~Claimed();
    };

    void readLoop(Claimed* c);

    // Negative and decreasing, mirroring the android synthetic-id space so they
    // never collide with positive SDL ids.
    std::atomic<int> nextSyntheticId_{-1000};

    mutable std::mutex mtx_;
    std::map<int, std::unique_ptr<Claimed>> claimed_;
};

} // namespace dish::source::usb
