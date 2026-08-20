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
// keeps the pad on SDL. Enumeration therefore never touches the node: it reads
// the world-readable sysfs attributes instead, so a pad the user cannot open
// still enumerates and the missing rule surfaces as that claim failure rather
// than as a device that silently never appears.

#pragma once

#include "source/usb/UsbDeviceGateway.h"

#include "core/input/UsbHidLayout.h"
#include "core/input/UsbReportParsers.h"

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dish::source::usb {

namespace detail {

// The bus/vendor/product a hidraw node's parent HID device publishes in its
// sysfs uevent as HID_ID=<bus>:<vendor>:<product> — the same three fields
// HIDIOCGRAWINFO returns, but readable without opening the node.
// core/input/HidTransport reads the same line for its bus-only Bluetooth
// classification; enumeration needs the vendor and product too.
struct HidIds {
    std::uint32_t bus = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t productId = 0;
};

// The value of `key=` in a uevent's KEY=VALUE text; empty when the key is absent.
inline std::string ueventValue(std::string_view uevent, std::string_view key) {
    for (std::size_t pos = 0; pos < uevent.size();) {
        std::size_t end = uevent.find('\n', pos);
        if (end == std::string_view::npos) { end = uevent.size(); }
        const std::string_view line = uevent.substr(pos, end - pos);
        if (line.rfind(key, 0) == 0 && line.size() > key.size() && line[key.size()] == '=') {
            return std::string(line.substr(key.size() + 1));
        }
        pos = end + 1;
    }
    return {};
}

// nullopt unless all three hex fields are there and well-formed, so a device
// whose identity cannot be read is skipped rather than guessed at.
inline std::optional<HidIds> parseHidIds(std::string_view uevent) {
    const std::string value = ueventValue(uevent, "HID_ID");
    std::string_view rest(value);
    const auto next = [&rest](std::uint32_t& out) {
        const std::size_t sep = rest.find(':');
        const std::string_view token = rest.substr(0, sep);
        const char* const last = token.data() + token.size();
        const auto res = std::from_chars(token.data(), last, out, 16);
        rest = sep == std::string_view::npos ? std::string_view{} : rest.substr(sep + 1);
        return res.ec == std::errc{} && res.ptr == last;
    };
    HidIds ids;
    if (!next(ids.bus) || !next(ids.vendorId) || !next(ids.productId)) { return std::nullopt; }
    return ids;
}

constexpr std::uint16_t kUsagePageGenericDesktop = 0x01;
constexpr std::uint16_t kUsageJoystick = 0x04;
constexpr std::uint16_t kUsageGamepad = 0x05;
// The Steam Controller's game interface is vendor-defined HID with no gamepad
// usages, so it is admitted by model rather than by shape.
constexpr std::uint16_t kUsagePageVendor = 0xFF00;

// The top-level application collection's usage page + usage, which is what the
// admission rule keys on. Walks short items only; a long item (0xFE) ends the
// scan because nothing we admit uses one.
inline bool topLevelUsage(const std::uint8_t* desc, std::size_t len, std::uint16_t& outPage,
                          std::uint16_t& outUsage) {
    std::uint32_t page = 0;
    std::uint32_t usage = 0;
    bool sawUsage = false;
    for (std::size_t i = 0; i < len;) {
        const std::uint8_t prefix = desc[i];
        if (prefix == 0xFE) { return false; }
        const std::uint8_t tag = prefix & 0xFC;
        std::uint8_t size = prefix & 0x03;
        if (size == 3) { size = 4; }
        if (i + 1 + size > len) { return false; }
        std::uint32_t data = 0;
        for (std::uint8_t b = 0; b < size; b++) {
            data |= static_cast<std::uint32_t>(desc[i + 1 + b]) << (8 * b);
        }
        if (tag == 0x04) { // Usage Page (global)
            page = data;
        } else if (tag == 0x08 && !sawUsage) { // Usage (local)
            usage = data;
            sawUsage = true;
        } else if (tag == 0xA0) { // Collection
            if (data == 0x01) {   // Application
                outPage = static_cast<std::uint16_t>(page);
                outUsage = static_cast<std::uint16_t>(usage);
                return true;
            }
        }
        i += 1u + size;
    }
    return false;
}

inline bool collectionMatchesParser(std::uint16_t page, std::uint16_t usage,
                                    input::usbparse::HidParser parser) {
    if (parser == input::usbparse::HidParser::SteamController) { return page == kUsagePageVendor; }
    return page == kUsagePageGenericDesktop && (usage == kUsageGamepad || usage == kUsageJoystick);
}

} // namespace detail

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
