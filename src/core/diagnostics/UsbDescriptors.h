// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A USB pad's hardware as its own descriptors state it (docs/diagnostics-bundle.md, `hardware` and
// `capture` in format usb-descriptors-v1): the device descriptor, every interface of the
// configuration with the endpoints that follow it, and the claimed interface's HID report
// descriptor. The bundle names a pad by the SHA-256 of its raw USB descriptors, which one pad and
// firmware give alike on every platform. Pure and allocation-bounded: a descriptor that lies about
// its length ends the walk rather than reading past the bytes.
//
// The strings are the manufacturer's and the product's, by their descriptor index. The serial
// number identifies one unit: no index is read for it, and a string that shares its index is
// left out with it.

#pragma once

#include "core/diagnostics/Json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::diagnostics {

enum class EndpointType : std::uint8_t { Control, Isochronous, Bulk, Interrupt };

struct UsbEndpoint {
    std::uint8_t address = 0;
    EndpointType type = EndpointType::Control;
    std::uint16_t maxPacketSize = 0;
    std::uint8_t interval = 0;

    bool operator==(const UsbEndpoint&) const = default;
    bool isIn() const;
};

// One interface descriptor; each alternate setting of an interface is an entry of its own.
struct UsbInterface {
    std::uint8_t number = 0;
    std::uint8_t alternate = 0;
    std::uint8_t interfaceClass = 0;
    std::uint8_t subclass = 0;
    std::uint8_t protocol = 0;
    std::vector<UsbEndpoint> endpoints;

    bool operator==(const UsbInterface&) const = default;
};

struct UsbDeviceDescriptor {
    std::uint16_t bcdUsb = 0;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::uint16_t bcdDevice = 0;
    std::uint8_t manufacturerIndex = 0;
    std::uint8_t productIndex = 0;
    std::uint8_t serialNumberIndex = 0;
    std::uint8_t configurations = 0;

    bool operator==(const UsbDeviceDescriptor&) const = default;
};

using DescriptorBytes = std::vector<std::uint8_t>;

// A descriptors file as Linux's sysfs and Android's rawDescriptors serve it: the device descriptor
// (its own bLength), then every configuration with its subordinate descriptors.
struct RawDescriptors {
    DescriptorBytes device;
    DescriptorBytes configuration;

    bool operator==(const RawDescriptors&) const = default;
};

RawDescriptors splitDescriptors(const std::uint8_t* raw, std::size_t len);

std::optional<UsbDeviceDescriptor> parseDeviceDescriptor(const DescriptorBytes& device);

// Every interface descriptor in the configuration bytes, in order, each with the endpoint
// descriptors that follow it up to the next interface.
std::vector<UsbInterface> interfacesOf(const DescriptorBytes& configuration);

// Where a report descriptor's bytes came from: read off the pad itself (Android's control
// transfer, the hub request on Windows), the operating system's copy, which a HID driver may have
// rewritten (Linux's sysfs report_descriptor, after the driver's report_fixup; IOKit's
// kIOHIDReportDescriptorKey), or a parsed form the client turned back into a descriptor.
enum class DescriptorSource : std::uint8_t { Device, Os, Reconstructed };

struct HidReportDescriptor {
    int interfaceNumber = 0;
    DescriptorBytes bytes;
    DescriptorSource source = DescriptorSource::Device;

    bool operator==(const HidReportDescriptor&) const = default;
};

// One string descriptor's text, by its index.
using UsbString = std::pair<int, std::string>;

// What a gateway reads about a pad once, when it first enumerates it. Every field is empty where
// the platform could not read it.
struct UsbHardwareFacts {
    std::optional<std::uint16_t> bcdDevice;
    std::vector<UsbInterface> interfaces;
    RawDescriptors raw;
    std::vector<UsbString> strings;
    std::optional<HidReportDescriptor> reportDescriptor;

    bool operator==(const UsbHardwareFacts&) const = default;
};

// The manufacturer and product strings a capture may carry, by the device descriptor's indices:
// index 0 (no string) and the serial number's index never.
std::vector<UsbString> captureStrings(const UsbDeviceDescriptor& device,
                                      const std::string& manufacturer, const std::string& product);

std::string hexOf(const std::uint8_t* bytes, std::size_t len);
std::string hexOf(const DescriptorBytes& bytes);
// Lowercase SHA-256 of the bytes; empty for none.
std::string sha256Hex(const DescriptorBytes& bytes);
// "0107" for 1.07: four hex digits, as the bundle's bcdDevice reads.
std::string bcdText(std::uint16_t bcd);

std::string_view endpointTypeToken(EndpointType type);
std::string_view descriptorSourceToken(DescriptorSource source);

// `hardware.interfaces` with their endpoints.
Json interfacesJson(const std::vector<UsbInterface>& interfaces);
// `hardware.descriptorSha256`: the SHA-256 of the device descriptor followed by every
// configuration in index order, as the bus delivered them (Android's getRawDescriptors(), Linux's
// sysfs `descriptors`, the Windows hub request); none for a pad with no raw descriptors.
std::optional<std::string> rawDescriptorsSha256(const UsbHardwareFacts& hardware);
Json descriptorSha256Json(const UsbHardwareFacts& hardware);

} // namespace dish::diagnostics
