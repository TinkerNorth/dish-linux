// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/UsbSysfs.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace dish::source::usb {

namespace {

// The descriptors attribute is at most the device descriptor and 65535 bytes of configuration
// (drivers/usb/core/sysfs.c, BIN_ATTR_RO(descriptors, 18 + 65535)).
constexpr std::size_t kMaxDescriptorsBytes = 18 + 65535;

std::string firstLine(const std::string& path) {
    std::ifstream in(path);
    if (!in) { return {}; }
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
    return line;
}

diagnostics::DescriptorBytes fileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return {}; }
    diagnostics::DescriptorBytes out;
    char byte = 0;
    while (out.size() < kMaxDescriptorsBytes && in.get(byte)) {
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return out;
}

std::optional<std::uint16_t> hexWord(const std::string& text) {
    std::uint16_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value, 16);
    const bool whole = !text.empty() && parsed.ec == std::errc{} && parsed.ptr == last;
    if (!whole) { return std::nullopt; }
    return value;
}

// "usb" and digits: the kernel's name for a host controller's root hub.
bool isRootHubName(std::string_view name) {
    constexpr std::string_view kPrefix = "usb";
    if (name.size() <= kPrefix.size() || name.rfind(kPrefix, 0) != 0) { return false; }
    const std::string_view number = name.substr(kPrefix.size());
    return std::all_of(number.begin(), number.end(),
                       [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
}

} // namespace

diagnostics::UsbHardwareFacts readUsbHardware(const std::string& usbDeviceDir, int interfaceNumber,
                                              diagnostics::DescriptorBytes reportDescriptor) {
    diagnostics::UsbHardwareFacts facts;
    const diagnostics::DescriptorBytes all = fileBytes(usbDeviceDir + "/descriptors");
    facts.raw = diagnostics::splitDescriptors(all.data(), all.size());
    facts.interfaces = diagnostics::interfacesOf(facts.raw.configuration);
    const std::optional<diagnostics::UsbDeviceDescriptor> device =
        diagnostics::parseDeviceDescriptor(facts.raw.device);
    facts.bcdDevice = hexWord(firstLine(usbDeviceDir + "/bcdDevice"));
    if (!facts.bcdDevice.has_value() && device.has_value()) { facts.bcdDevice = device->bcdDevice; }
    if (device.has_value()) {
        facts.strings =
            diagnostics::captureStrings(*device, firstLine(usbDeviceDir + "/manufacturer"),
                                        firstLine(usbDeviceDir + "/product"));
    }
    if (!reportDescriptor.empty()) {
        facts.reportDescriptor = diagnostics::HidReportDescriptor{
            interfaceNumber, std::move(reportDescriptor), diagnostics::DescriptorSource::Device};
    }
    return facts;
}

std::optional<int> countRootHubs(const std::string& devicesDir) {
    std::error_code error;
    std::filesystem::directory_iterator entry(devicesDir, error);
    if (error) { return std::nullopt; }
    int hubs = 0;
    // The error_code forms throughout: a directory that changes under the walk ends it, it never
    // throws into the caller.
    for (const std::filesystem::directory_iterator end; entry != end; entry.increment(error)) {
        if (error) { break; }
        if (isRootHubName(entry->path().filename().string())) { ++hubs; }
    }
    return hubs;
}

} // namespace dish::source::usb
