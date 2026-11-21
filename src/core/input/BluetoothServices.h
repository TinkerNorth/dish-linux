// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// `bluetooth.transportLayer` from the services a remote device lists (docs/diagnostics-bundle.md,
// "Every controller path"): HID over L2CAP is the HID profile, service 0x1124, and HID over GATT
// is 0x1812. A pad that lists one of the two runs its HID over that link; one that lists both or
// neither does not say, and neither does a system that could not be asked. BlueZ's
// org.bluez.Device1.UUIDs and Android's BluetoothDevice.getUuids() both list them in the
// Bluetooth base UUID's 128-bit form.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::input {

enum class TransportLayer : std::uint8_t { Unknown, Classic, LowEnergy };

inline constexpr std::uint16_t kHidProfileService = 0x1124;
inline constexpr std::uint16_t kHidOverGattService = 0x1812;

namespace detail {

// "0000xxxx-0000-1000-8000-00805f9b34fb", the base UUID with the 16-bit id in its first group.
inline constexpr std::string_view kBaseUuidTail = "-0000-1000-8000-00805f9b34fb";
inline constexpr std::size_t kUuidTextLength = 36;
inline constexpr std::size_t kShortIdOffset = 4;
inline constexpr std::size_t kShortIdDigits = 4;
inline constexpr std::size_t kBaseUuidTailOffset = 8;

inline std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

inline std::string shortIdText(std::uint16_t shortId) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(kShortIdDigits, '0');
    for (std::size_t i = 0; i < kShortIdDigits; ++i) {
        const auto shift = static_cast<unsigned>(4 * (kShortIdDigits - 1 - i));
        out[i] = kDigits[(static_cast<unsigned>(shortId) >> shift) & 0x0FU];
    }
    return out;
}

} // namespace detail

// Whether `uuid` is the 16-bit service `shortId` in the base UUID's form, in either case.
inline bool isBaseUuidService(std::string_view uuid, std::uint16_t shortId) {
    if (uuid.size() != detail::kUuidTextLength) { return false; }
    const std::string text = detail::lowered(uuid);
    const bool zeroPrefix = text.compare(0, detail::kShortIdOffset, "0000") == 0;
    const bool idMatches = text.compare(detail::kShortIdOffset, detail::kShortIdDigits,
                                        detail::shortIdText(shortId)) == 0;
    const bool tailMatches = text.compare(detail::kBaseUuidTailOffset, std::string_view::npos,
                                          detail::kBaseUuidTail) == 0;
    return zeroPrefix && idMatches && tailMatches;
}

inline bool listsService(const std::vector<std::string>& uuids, std::uint16_t service) {
    return std::any_of(uuids.begin(), uuids.end(), [service](const std::string& uuid) {
        return isBaseUuidService(uuid, service);
    });
}

// 0x1124 alone is classic, 0x1812 alone is LE, both or neither unknown; a system that could not be
// asked (no list) is unknown too.
inline TransportLayer transportFromServices(const std::optional<std::vector<std::string>>& uuids) {
    if (!uuids.has_value()) { return TransportLayer::Unknown; }
    const bool classic = listsService(*uuids, kHidProfileService);
    const bool lowEnergy = listsService(*uuids, kHidOverGattService);
    if (classic == lowEnergy) { return TransportLayer::Unknown; }
    return classic ? TransportLayer::Classic : TransportLayer::LowEnergy;
}

// The bundle's word for a link; empty where it is unknown, which the file writes as null.
inline std::string_view transportLayerToken(TransportLayer layer) {
    switch (layer) {
    case TransportLayer::Classic:
        return "classic";
    case TransportLayer::LowEnergy:
        return "le";
    case TransportLayer::Unknown:
        return {};
    }
    return {};
}

// A remote address as HID_UNIQ carries it ("aa:bb:cc:dd:ee:ff", BlueZ's and the kernel's form),
// uppercased the way BlueZ spells it in a device's object path; none for anything else, and for
// the all-zero address BlueZ writes when it could not read the peer's.
inline std::optional<std::string> remoteAddressOf(std::string_view uniq) {
    constexpr std::size_t kAddressLength = 17;
    if (uniq.size() != kAddressLength) { return std::nullopt; }
    std::string out(uniq);
    bool allZero = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const bool separator = i % 3 == 2;
        const auto ch = static_cast<unsigned char>(out[i]);
        if (separator ? ch != ':' : std::isxdigit(ch) == 0) { return std::nullopt; }
        if (!separator) {
            allZero = allZero && ch == '0';
            out[i] = static_cast<char>(std::toupper(ch));
        }
    }
    if (allZero) { return std::nullopt; }
    return out;
}

} // namespace dish::input
