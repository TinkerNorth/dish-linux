// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Classify a device node as USB- or Bluetooth-attached. A Bluetooth-connected
// pad must not become a USB-direct claim candidate: the BT report layout
// differs per model (a DS4 streams the short 0x01 report until a feature-report
// handshake), so a raw-HID claim over the BT interface decodes garbage.
//
// The answer comes from the HID bus in sysfs rather than the path text, so it
// is a fact rather than a naming convention. An unresolvable path reads as
// not-Bluetooth, which fails safe to the wired presentation.

#pragma once

#include <fstream>
#include <string>
#include <string_view>

namespace dish::input {

namespace detail {

// HID_ID=<bus>:<vendor>:<product>, bus 0005 == BUS_BLUETOOTH.
inline bool ueventSaysBluetooth(const std::string& ueventPath) {
    std::ifstream in(ueventPath);
    if (!in) { return false; }
    std::string line;
    while (std::getline(in, line)) {
        constexpr std::string_view kPrefix = "HID_ID=";
        if (line.rfind(kPrefix, 0) != 0) { continue; }
        const std::string bus = line.substr(kPrefix.size(), 4);
        return bus == "0005";
    }
    return false;
}

inline std::string nodeName(std::string_view path, std::string_view prefix) {
    if (path.rfind(prefix, 0) != 0) { return {}; }
    return std::string(path.substr(prefix.size()));
}

} // namespace detail

// SDL reports the hidraw node for HIDAPI-backed pads and the evdev node for the
// kernel joystick backend; both hang off the HID device whose uevent carries
// the bus.
inline bool isBluetoothHidDevicePath(std::string_view path) {
    if (path.empty()) { return false; }

    if (const std::string hidraw = detail::nodeName(path, "/dev/hidraw"); !hidraw.empty()) {
        return detail::ueventSaysBluetooth("/sys/class/hidraw/hidraw" + hidraw + "/device/uevent");
    }
    if (const std::string event = detail::nodeName(path, "/dev/input/event"); !event.empty()) {
        // input -> hid is one level up from the input device's own directory.
        return detail::ueventSaysBluetooth("/sys/class/input/event" + event +
                                           "/device/device/uevent");
    }
    return false;
}

} // namespace dish::input
