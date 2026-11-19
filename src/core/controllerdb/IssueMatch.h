// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which of a record's `support.issues` show on a pad's card here
// (docs/controller-db.md section 4.1): the issue is `open`, its transport is
// the pad's, and its `device` and `os` are this machine's or absent. `device`
// is the machine's model as the bundle names it ("AYN Thor"), compared without
// case or surrounding space; an `os` map is keyed by platform, and only its
// `linux` entry, read as a major version ("6", or "6.8" as a string), can
// match here. `fixed` and `wontfix` issues are history and never show.
//
// The contract does not say what a Linux major version is. There is no single
// distribution release to name, so it is the kernel's: the one version every
// Linux machine has, and the one a hidraw or usbhid behaviour changes with
// (tests/PARITY.md says so).

#pragma once

#include "core/controllerdb/ControllerRecord.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::controllerdb {

struct HostFacts {
    std::string model;
    // The running kernel's major version, 6 for "6.8.0-45-generic"; 0 when unknown.
    int kernelMajor = 0;
};

namespace detail {

inline std::string foldedForMatch(std::string_view text) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && isSpace(static_cast<unsigned char>(text[first]))) { ++first; }
    while (last > first && isSpace(static_cast<unsigned char>(text[last - 1]))) { --last; }
    std::string out(text.substr(first, last - first));
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

inline bool deviceMatches(const std::optional<std::string>& device, const HostFacts& host) {
    if (!device.has_value()) { return true; }
    return foldedForMatch(*device) == foldedForMatch(host.model);
}

// The leading whole number of a version, "11" of "11.0.22631"; -1 when it starts with none.
inline int majorVersionOf(std::string_view version) {
    int major = -1;
    for (const char ch : version) {
        if (ch < '0' || ch > '9') { break; }
        major = (major < 0 ? 0 : major * 10) + (ch - '0');
    }
    return major;
}

inline bool osMatches(const std::vector<IssueOs>& os, const HostFacts& host) {
    if (os.empty()) { return true; }
    return std::any_of(os.begin(), os.end(), [&host](const IssueOs& entry) {
        return entry.name == kPlatformName && majorVersionOf(entry.version) == host.kernelMajor;
    });
}

} // namespace detail

inline constexpr std::string_view kOpenStatus = "open";

inline bool issueApplies(const Issue& issue, Transport padTransport, const HostFacts& host) {
    const bool open = issue.status == kOpenStatus;
    const bool sameTransport = issue.transport == padTransport;
    const bool thisMachine = detail::deviceMatches(issue.device, host);
    const bool thisSystem = detail::osMatches(issue.os, host);
    return open && sameTransport && thisMachine && thisSystem;
}

inline std::vector<Issue> applicableIssues(const ControllerRecord& record, Transport padTransport,
                                           const HostFacts& host) {
    std::vector<Issue> out;
    std::copy_if(record.issues.begin(), record.issues.end(), std::back_inserter(out),
                 [padTransport, &host](const Issue& issue) {
                     return issueApplies(issue, padTransport, host);
                 });
    return out;
}

} // namespace dish::controllerdb
