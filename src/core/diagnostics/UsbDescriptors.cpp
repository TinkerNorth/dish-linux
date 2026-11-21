// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/diagnostics/UsbDescriptors.h"

#include <sodium.h>

#include <algorithm>
#include <array>

namespace dish::diagnostics {

namespace {

constexpr std::uint8_t kDeviceDescriptorType = 0x01;
constexpr std::uint8_t kInterfaceDescriptorType = 0x04;
constexpr std::uint8_t kEndpointDescriptorType = 0x05;
constexpr std::size_t kDeviceDescriptorBytes = 18;
constexpr std::size_t kInterfaceDescriptorBytes = 9;
constexpr std::size_t kEndpointDescriptorBytes = 7;
constexpr std::uint8_t kEndpointDirectionIn = 0x80;
constexpr std::uint8_t kTransferTypeMask = 0x03;
constexpr std::uint16_t kMaxPacketSizeMask = 0x07FF;
constexpr std::size_t kHeaderBytes = 2;

std::uint16_t le16(const std::uint8_t* at) {
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
}

// One descriptor's bytes as the walk met it; empty when the next header lies about its length.
struct DescriptorView {
    const std::uint8_t* bytes = nullptr;
    std::size_t length = 0;
    std::uint8_t type = 0;
};

DescriptorView descriptorAt(const DescriptorBytes& all, std::size_t at) {
    const bool headerFits = at + kHeaderBytes <= all.size();
    if (!headerFits) { return {}; }
    const std::size_t length = all[at];
    const bool bodyFits = length >= kHeaderBytes && at + length <= all.size();
    if (!bodyFits) { return {}; }
    return DescriptorView{all.data() + at, length, all[at + 1]};
}

EndpointType endpointTypeOf(std::uint8_t attributes) {
    constexpr std::array<EndpointType, 4> kTypes{EndpointType::Control, EndpointType::Isochronous,
                                                 EndpointType::Bulk, EndpointType::Interrupt};
    return kTypes[static_cast<std::size_t>(attributes & kTransferTypeMask)];
}

UsbInterface interfaceFrom(const DescriptorView& d) {
    UsbInterface out;
    out.number = d.bytes[2];
    out.alternate = d.bytes[3];
    out.interfaceClass = d.bytes[5];
    out.subclass = d.bytes[6];
    out.protocol = d.bytes[7];
    return out;
}

// The packet size's low eleven bits: the two above them count extra transactions per microframe.
UsbEndpoint endpointFrom(const DescriptorView& d) {
    UsbEndpoint out;
    out.address = d.bytes[2];
    out.type = endpointTypeOf(d.bytes[3]);
    out.maxPacketSize = static_cast<std::uint16_t>(le16(d.bytes + 4) & kMaxPacketSizeMask);
    out.interval = d.bytes[6];
    return out;
}

void addEndpoint(std::vector<UsbInterface>& interfaces, const DescriptorView& d) {
    const bool belongsToAnInterface = !interfaces.empty();
    const bool whole = d.length >= kEndpointDescriptorBytes;
    if (belongsToAnInterface && whole) { interfaces.back().endpoints.push_back(endpointFrom(d)); }
}

// A string the device names by a real index, unless that index is the serial number's.
std::optional<std::string> stringAt(const UsbDeviceDescriptor& device, std::uint8_t index,
                                    const std::string& text) {
    const bool named = index != 0 && !text.empty();
    const bool isTheSerialNumber = index == device.serialNumberIndex;
    if (!named || isTheSerialNumber) { return std::nullopt; }
    return text;
}

} // namespace

bool UsbEndpoint::isIn() const { return (address & kEndpointDirectionIn) != 0; }

// At 18 bytes whatever bLength says: sysfs writes sizeof(struct usb_device_descriptor) before the
// configurations (drivers/usb/core/sysfs.c, descriptors_read).
RawDescriptors splitDescriptors(const std::uint8_t* raw, std::size_t len) {
    if (raw == nullptr || len == 0) { return {}; }
    const std::size_t deviceLength = std::min(kDeviceDescriptorBytes, len);
    RawDescriptors out;
    out.device.assign(raw, raw + deviceLength);
    out.configuration.assign(raw + deviceLength, raw + len);
    return out;
}

std::optional<UsbDeviceDescriptor> parseDeviceDescriptor(const DescriptorBytes& device) {
    const bool whole =
        device.size() >= kDeviceDescriptorBytes && device[0] >= kDeviceDescriptorBytes;
    if (!whole || device[1] != kDeviceDescriptorType) { return std::nullopt; }
    UsbDeviceDescriptor out;
    out.bcdUsb = le16(device.data() + 2);
    out.vendorId = le16(device.data() + 8);
    out.productId = le16(device.data() + 10);
    out.bcdDevice = le16(device.data() + 12);
    out.manufacturerIndex = device[14];
    out.productIndex = device[15];
    out.serialNumberIndex = device[16];
    out.configurations = device[17];
    return out;
}

std::vector<UsbInterface> interfacesOf(const DescriptorBytes& configuration) {
    std::vector<UsbInterface> interfaces;
    std::size_t at = 0;
    for (DescriptorView d = descriptorAt(configuration, at); d.bytes != nullptr;
         d = descriptorAt(configuration, at)) {
        const bool isInterface =
            d.type == kInterfaceDescriptorType && d.length >= kInterfaceDescriptorBytes;
        if (isInterface) { interfaces.push_back(interfaceFrom(d)); }
        if (d.type == kEndpointDescriptorType) { addEndpoint(interfaces, d); }
        at += d.length;
    }
    return interfaces;
}

CaptureStrings captureStrings(const UsbDeviceDescriptor& device, const std::string& manufacturer,
                              const std::string& product) {
    return CaptureStrings{stringAt(device, device.manufacturerIndex, manufacturer),
                          stringAt(device, device.productIndex, product)};
}

std::string hexOf(const std::uint8_t* bytes, std::size_t len) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kDigits[static_cast<std::size_t>(bytes[i] >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(bytes[i] & 0x0F)]);
    }
    return out;
}

std::string hexOf(const DescriptorBytes& bytes) { return hexOf(bytes.data(), bytes.size()); }

std::string sha256Hex(const DescriptorBytes& bytes) {
    if (bytes.empty()) { return {}; }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), bytes.data(), bytes.size());
    return hexOf(digest.data(), digest.size());
}

std::string bcdText(std::uint16_t bcd) {
    const std::array<std::uint8_t, 2> bigEndian{static_cast<std::uint8_t>(bcd >> 8),
                                                static_cast<std::uint8_t>(bcd & 0xFF)};
    return hexOf(bigEndian.data(), bigEndian.size());
}

std::string_view endpointTypeToken(EndpointType type) {
    switch (type) {
    case EndpointType::Control:
        return "control";
    case EndpointType::Isochronous:
        return "isochronous";
    case EndpointType::Bulk:
        return "bulk";
    case EndpointType::Interrupt:
        return "interrupt";
    }
    return "control";
}

std::string_view descriptorSourceToken(DescriptorSource source) {
    switch (source) {
    case DescriptorSource::Device:
        return "device";
    case DescriptorSource::Os:
        return "os";
    case DescriptorSource::Reconstructed:
        return "reconstructed";
    }
    return "device";
}

Json interfacesJson(const std::vector<UsbInterface>& interfaces) {
    Json out = Json::array();
    for (const UsbInterface& intf : interfaces) {
        Json endpoints = Json::array();
        for (const UsbEndpoint& ep : intf.endpoints) {
            Json endpoint = Json::object();
            endpoint.set("address", hexOf(&ep.address, 1));
            endpoint.set("type", endpointTypeToken(ep.type));
            endpoint.set("maxPacketSize", static_cast<int>(ep.maxPacketSize));
            endpoint.set("interval", static_cast<int>(ep.interval));
            endpoints.push(std::move(endpoint));
        }
        Json entry = Json::object();
        entry.set("number", static_cast<int>(intf.number));
        entry.set("alternate", static_cast<int>(intf.alternate));
        entry.set("class", hexOf(&intf.interfaceClass, 1));
        entry.set("subclass", hexOf(&intf.subclass, 1));
        entry.set("protocol", hexOf(&intf.protocol, 1));
        entry.set("endpoints", std::move(endpoints));
        out.push(std::move(entry));
    }
    return out;
}

std::optional<std::string> rawDescriptorsSha256(const UsbHardwareFacts& hardware) {
    DescriptorBytes raw = hardware.raw.device;
    raw.insert(raw.end(), hardware.raw.configuration.begin(), hardware.raw.configuration.end());
    if (raw.empty()) { return std::nullopt; }
    return sha256Hex(raw);
}

Json descriptorSha256Json(const UsbHardwareFacts& hardware) {
    return Json::orNull(rawDescriptorsSha256(hardware));
}

} // namespace dish::diagnostics
