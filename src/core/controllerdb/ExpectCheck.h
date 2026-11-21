// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A record's `expect` against what the Direct path observed at attach
// (docs/controller-db.md section 4.2). Each disagreement is one line in the
// contract's form, "endpoints.out: expected true, found false", which the
// Diagnostics card and the bundle carry verbatim. A side either party does not
// know is not compared.
//
// What is observed is the claimed interface's endpoints, as sysfs lists them and
// as dish-android's check reads its claim's endpoint pair.

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
