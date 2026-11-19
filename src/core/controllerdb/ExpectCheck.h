// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A record's `expect` against what the Direct path observed at attach
// (docs/controller-db.md section 4.2). Each disagreement is one line in the
// contract's form, "endpoints.out: expected true, found false", which the
// Diagnostics card and the bundle carry verbatim. A side either party does not
// know is not compared.
//
// hidraw hands back the interface's report descriptor, and that is what is
// observed here: an Input main item stands for the IN endpoint, an Output main
// item for the OUT one. usbhid sends an output report over the interrupt OUT
// pipe when the interface has one and as a SET_REPORT(Output) otherwise, so
// the descriptor's promise to take output reports is the observable, not the
// endpoint itself (tests/PARITY.md says so).

#pragma once

#include "core/controllerdb/ControllerRecord.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::controllerdb {

struct ObservedHardware {
    std::optional<bool> endpointIn;
    std::optional<bool> endpointOut;

    bool operator==(const ObservedHardware&) const = default;
};

namespace detail {

constexpr std::uint8_t kLongItemPrefix = 0xFE;
constexpr std::uint8_t kTagMask = 0xFC;
constexpr std::uint8_t kSizeMask = 0x03;
constexpr std::uint8_t kFourByteSizeCode = 3;
constexpr std::uint8_t kInputItemTag = 0x80;
constexpr std::uint8_t kOutputItemTag = 0x90;

// The byte length of the item that starts at `at`, prefix included; a long item carries its data
// size in the byte after the prefix and two header bytes after that.
inline std::size_t itemLength(const std::uint8_t* descriptor, std::size_t len, std::size_t at) {
    const std::uint8_t prefix = descriptor[at];
    if (prefix == kLongItemPrefix) {
        const bool sized = at + 1 < len;
        return sized ? std::size_t{3} + descriptor[at + 1] : len - at;
    }
    const std::uint8_t sizeCode = prefix & kSizeMask;
    const std::size_t dataBytes = sizeCode == kFourByteSizeCode ? 4 : sizeCode;
    return 1 + dataBytes;
}

} // namespace detail

// What a report descriptor promises: Input items mean IN reports, Output items mean the interface
// takes output reports. An empty descriptor observes nothing.
inline ObservedHardware observedFromReportDescriptor(const std::uint8_t* descriptor,
                                                     std::size_t len) {
    if (descriptor == nullptr || len == 0) { return ObservedHardware{}; }
    bool hasInput = false;
    bool hasOutput = false;
    std::size_t at = 0;
    while (at < len) {
        const std::uint8_t tag = descriptor[at] & detail::kTagMask;
        const bool shortItem = descriptor[at] != detail::kLongItemPrefix;
        hasInput = hasInput || (shortItem && tag == detail::kInputItemTag);
        hasOutput = hasOutput || (shortItem && tag == detail::kOutputItemTag);
        at += detail::itemLength(descriptor, len, at);
    }
    return ObservedHardware{hasInput, hasOutput};
}

namespace detail {

inline std::optional<bool> expectedPresence(Expectation expectation) {
    switch (expectation) {
    case Expectation::Present:
        return true;
    case Expectation::Absent:
        return false;
    case Expectation::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

inline std::string_view boolWord(bool value) { return value ? "true" : "false"; }

inline void compareOne(std::string_view field, Expectation expected, std::optional<bool> found,
                       std::vector<std::string>& out) {
    const std::optional<bool> wanted = expectedPresence(expected);
    if (!wanted.has_value() || !found.has_value()) { return; }
    if (*wanted == *found) { return; }
    std::string line(field);
    line += ": expected ";
    line += boolWord(*wanted);
    line += ", found ";
    line += boolWord(*found);
    out.push_back(std::move(line));
}

} // namespace detail

inline std::vector<std::string> expectMismatches(const Expect& expect,
                                                 const ObservedHardware& observed) {
    std::vector<std::string> out;
    detail::compareOne("endpoints.in", expect.endpointIn, observed.endpointIn, out);
    detail::compareOne("endpoints.out", expect.endpointOut, observed.endpointOut, out);
    return out;
}

} // namespace dish::controllerdb
