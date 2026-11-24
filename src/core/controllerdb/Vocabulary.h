// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database's fixed vocabulary (docs/controller-db.md section
// 4.1), one table per word list, read both ways so a token and its enum cannot
// drift apart.

#pragma once

#include "core/Bits.h"
#include "core/controllerdb/ControllerRecord.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dish::controllerdb {

template <class E> struct Token {
    E value;
    std::string_view text;
};

template <class E, std::size_t N>
constexpr std::optional<E> fromToken(const std::array<Token<E>, N>& table, std::string_view text) {
    for (const Token<E>& entry : table) {
        if (entry.text == text) { return entry.value; }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
constexpr std::string_view toToken(const std::array<Token<E>, N>& table, E value) {
    for (const Token<E>& entry : table) {
        if (entry.value == value) { return entry.text; }
    }
    return {};
}

inline constexpr std::array<Token<Family>, 12> kFamilyTokens{{
    {Family::Xinput360, "xinput360"},
    {Family::Xinput360Wireless, "xinput360Wireless"},
    {Family::XboxOneGip, "xboxOneGip"},
    {Family::DualShock4, "dualshock4"},
    {Family::DualSense, "dualsense"},
    {Family::SwitchProUsb, "switchProUsb"},
    {Family::SwitchN64Usb, "switchN64Usb"},
    {Family::Stadia, "stadia"},
    {Family::Hid, "hid"},
    {Family::SteamController, "steamController"},
    {Family::SteamTriton, "steamTriton"},
    {Family::SteamControllerBle, "steamControllerBle"},
}};

inline constexpr std::array<Token<Mapping>, 4> kMappingTokens{{
    {Mapping::Western, "western"},
    {Mapping::Switch, "switch"},
    {Mapping::EightBitDo, "eightbitdo"},
    {Mapping::PlayStationClassic, "playstationClassic"},
}};

inline constexpr std::array<Token<DirectRule>, 3> kDirectRuleTokens{{
    {DirectRule::Auto, "auto"},
    {DirectRule::OptIn, "optIn"},
    {DirectRule::WhenOsCannot, "whenOsCannot"},
}};

// The record's null route has no token; the parser reads a JSON null as OutputRoute::None.
inline constexpr std::array<Token<OutputRoute>, 2> kRouteTokens{{
    {OutputRoute::InterruptOut, "interruptOut"},
    {OutputRoute::SetReport, "setReport"},
}};

inline constexpr std::array<Token<InitKind>, 6> kInitTokens{{
    {InitKind::None, "none"},
    {InitKind::XboxOnePowerOn, "xboxOnePowerOn"},
    {InitKind::XboxOneS, "xboxOneS"},
    {InitKind::SwitchProHandshake, "switchProHandshake"},
    {InitKind::SteamQuiet, "steamQuiet"},
    {InitKind::SteamTritonQuiet, "steamTritonQuiet"},
}};

inline constexpr std::array<Token<Verification>, 5> kVerificationTokens{{
    {Verification::Verified, "verified"},
    {Verification::Imported, "imported"},
    {Verification::Crowd, "crowd"},
    {Verification::Guessed, "guessed"},
    {Verification::Flagged, "flagged"},
}};

inline constexpr std::array<Token<Transport>, 2> kTransportTokens{{
    {Transport::Usb, "usb"},
    {Transport::Bluetooth, "bluetooth"},
}};

// A word a later database may add reads as Unknown rather than failing the row, so the record
// still names its pad and shows its issues while the binary leaves its input and output alone.
inline Family familyFrom(std::string_view text) {
    return fromToken(kFamilyTokens, text).value_or(Family::Unknown);
}

inline Mapping mappingFrom(std::string_view text) {
    return fromToken(kMappingTokens, text).value_or(Mapping::Unknown);
}

inline InitKind initFrom(std::string_view text) {
    return fromToken(kInitTokens, text).value_or(InitKind::Unknown);
}

inline OutputRoute routeFrom(std::string_view text) {
    return fromToken(kRouteTokens, text).value_or(OutputRoute::Unknown);
}

inline std::string_view familyToken(Family family) { return toToken(kFamilyTokens, family); }

inline std::string_view verificationToken(Verification state) {
    return toToken(kVerificationTokens, state);
}

inline std::string_view transportToken(Transport transport) {
    return toToken(kTransportTokens, transport);
}

// The card's chip for a verification state (section 4.2): verified, flagged, and every unverified
// state in this client's existing "Layout guessed" vocabulary.
inline std::string_view verificationChipToken(Verification state) {
    switch (state) {
    case Verification::Verified:
        return "verified";
    case Verification::Flagged:
        return "flagged";
    case Verification::Imported:
    case Verification::Crowd:
    case Verification::Guessed:
        return "guessed";
    }
    return "guessed";
}

// The strings keys a record's words live under (section 5).
inline std::string controllerNameKey(std::string_view id) {
    return "controller." + std::string(id) + ".name";
}
inline std::string controllerHintKey(std::string_view id) {
    return "controller." + std::string(id) + ".hint";
}
inline std::string familyNameKey(Family family) {
    return "family." + std::string(familyToken(family)) + ".name";
}
inline std::string issueTitleKey(std::string_view issueId) {
    return "issue." + std::string(issueId) + ".title";
}
inline std::string issueBodyKey(std::string_view issueId) {
    return "issue." + std::string(issueId) + ".body";
}

namespace detail {

constexpr std::optional<std::uint16_t> hexWord(std::string_view text) {
    if (text.size() != 4) { return std::nullopt; }
    std::uint32_t value = 0;
    for (const char ch : text) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lowerHex = ch >= 'a' && ch <= 'f';
        if (!digit && !lowerHex) { return std::nullopt; }
        const std::uint32_t nibble = digit ? static_cast<std::uint32_t>(ch - '0')
                                           : static_cast<std::uint32_t>(ch - 'a' + 10);
        value = (value << 4U) | nibble;
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace detail

// "054c:0ce6", lowercase, exactly as the schema's pattern has it; anything else is no id.
constexpr std::optional<DeviceId> parseDeviceId(std::string_view text) {
    constexpr std::size_t kSeparatorAt = 4;
    constexpr std::size_t kLength = 9;
    if (text.size() != kLength || text[kSeparatorAt] != ':') { return std::nullopt; }
    const std::optional<std::uint16_t> vendor = detail::hexWord(text.substr(0, kSeparatorAt));
    const std::optional<std::uint16_t> product = detail::hexWord(text.substr(kSeparatorAt + 1));
    if (!vendor.has_value() || !product.has_value()) { return std::nullopt; }
    return DeviceId{*vendor, *product};
}

// Four lowercase hex digits, the form the bundle and the card write a vendor or product in.
inline std::string hexWordText(std::uint16_t word) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(4, '0');
    for (int i = 3; i >= 0; --i) {
        const auto shift = static_cast<unsigned>(3 - i) * 4U;
        const auto nibble = static_cast<std::size_t>((bits::u32(word) >> shift) & 0xFU);
        out[static_cast<std::size_t>(i)] = kDigits[nibble];
    }
    return out;
}

inline std::string deviceIdText(const DeviceId& id) {
    return hexWordText(id.vendorId) + ":" + hexWordText(id.productId);
}

} // namespace dish::controllerdb
