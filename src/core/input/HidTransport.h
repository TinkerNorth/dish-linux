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

#include "core/input/BluetoothServices.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// Who made a Bluetooth HID device, which is what sysfs can say about its link. The kernel's HIDP
// parents a classic pad's HID device on its HCI connection (net/bluetooth/hidp/core.c sets
// hid->dev.parent to the connection, a directory named hciN:handle), and nothing else does. BlueZ
// makes a HID-over-GATT pad's through uhid, and from 5.73 a classic pad's too (its input plugin's
// UserspaceHID now defaults on), so a uhid device under /sys/devices/virtual/misc/uhid/ is either
// link and BlueZ's own service list for it decides. Only the verdict leaves; the path, the HCI
// handle and the address never do.
enum class BluetoothHidOrigin : std::uint8_t { NotBluetooth, HciConnection, Uhid, Other };

namespace detail {

inline bool allDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch)) != 0;
    });
}

// "hci0:256": the HCI connection's own directory name.
inline bool isHciConnectionName(std::string_view name) {
    constexpr std::string_view kPrefix = "hci";
    if (name.rfind(kPrefix, 0) != 0) { return false; }
    const std::size_t colon = name.find(':', kPrefix.size());
    if (colon == std::string_view::npos) { return false; }
    const std::string_view adapter = name.substr(kPrefix.size(), colon - kPrefix.size());
    const std::string_view handle = name.substr(colon + 1);
    return allDigits(adapter) && allDigits(handle);
}

inline std::string_view parentName(std::string_view hidDevicePath) {
    const std::size_t self = hidDevicePath.find_last_of('/');
    if (self == std::string_view::npos || self == 0) { return {}; }
    const std::size_t parent = hidDevicePath.find_last_of('/', self - 1);
    const std::size_t from = parent == std::string_view::npos ? 0 : parent + 1;
    return hidDevicePath.substr(from, self - from);
}

} // namespace detail

// `hidDevicePath` is the HID device's sysfs directory with its links resolved.
inline BluetoothHidOrigin bluetoothOriginOfHidDevice(bool busIsBluetooth,
                                                     std::string_view hidDevicePath) {
    if (!busIsBluetooth) { return BluetoothHidOrigin::NotBluetooth; }
    if (detail::isHciConnectionName(detail::parentName(hidDevicePath))) {
        return BluetoothHidOrigin::HciConnection;
    }
    const bool madeByUhid = hidDevicePath.find("/uhid/") != std::string_view::npos;
    return madeByUhid ? BluetoothHidOrigin::Uhid : BluetoothHidOrigin::Other;
}

// A classic pad from its HCI parent; a uhid device from BlueZ's org.bluez.Device1.UUIDs for the
// address it was made for (none: BlueZ could not be asked, or did not know the device).
inline TransportLayer transportLayerOf(BluetoothHidOrigin origin,
                                       const std::optional<std::vector<std::string>>& services) {
    switch (origin) {
    case BluetoothHidOrigin::HciConnection:
        return TransportLayer::Classic;
    case BluetoothHidOrigin::Uhid:
        return transportFromServices(services);
    case BluetoothHidOrigin::NotBluetooth:
    case BluetoothHidOrigin::Other:
        return TransportLayer::Unknown;
    }
    return TransportLayer::Unknown;
}

namespace detail {

// The HID device directory behind a hidraw or an evdev node, under `sysRoot`; empty for any other
// path.
inline std::string hidDirectoryOf(std::string_view path, const std::string& sysRoot) {
    if (const std::string hidraw = nodeName(path, "/dev/hidraw"); !hidraw.empty()) {
        return sysRoot + "/class/hidraw/hidraw" + hidraw + "/device";
    }
    if (const std::string event = nodeName(path, "/dev/input/event"); !event.empty()) {
        return sysRoot + "/class/input/event" + event + "/device/device";
    }
    return {};
}

inline std::string resolvedDirectory(const std::string& directory) {
    if (directory.empty()) { return {}; }
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(directory, error);
    return error ? std::string() : resolved.string();
}

inline std::string ueventField(const std::string& ueventPath, std::string_view key) {
    std::ifstream in(ueventPath);
    if (!in) { return {}; }
    std::string line;
    while (std::getline(in, line)) {
        const bool isTheKey = line.size() > key.size() && line.compare(0, key.size(), key) == 0 &&
                              line[key.size()] == '=';
        if (isTheKey) { return line.substr(key.size() + 1); }
    }
    return {};
}

} // namespace detail

// The same two node shapes isBluetoothHidDevicePath reads, under `sysRoot` ("/sys" outside a
// test). A path that resolves to nothing is not Bluetooth.
inline BluetoothHidOrigin bluetoothOriginOfHidPath(std::string_view path,
                                                   const std::string& sysRoot = "/sys") {
    const std::string resolved = detail::resolvedDirectory(detail::hidDirectoryOf(path, sysRoot));
    if (resolved.empty()) { return BluetoothHidOrigin::NotBluetooth; }
    const bool bluetooth = detail::ueventSaysBluetooth(resolved + "/uevent");
    return bluetoothOriginOfHidDevice(bluetooth, resolved);
}

// HID_UNIQ of the node's HID device: for a uhid device the remote address BlueZ made it for
// (BlueZ 5.72 profiles/input/device.c and hog-lib.c, master src/shared/uhid.c). Read when the
// support file is written, used to find the pad at BlueZ and dropped; never kept or written.
inline std::string hidUniqOfHidPath(std::string_view path, const std::string& sysRoot = "/sys") {
    const std::string resolved = detail::resolvedDirectory(detail::hidDirectoryOf(path, sysRoot));
    if (resolved.empty()) { return {}; }
    return detail::ueventField(resolved + "/uevent", "HID_UNIQ");
}

} // namespace dish::input
