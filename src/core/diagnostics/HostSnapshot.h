// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What each host was able to say about itself for the support report
// (docs/diagnostics-bundle.md, "The hosts, from both ends"). A Satellite of
// 2.3.0 or later answers GET /api/connections/{connectionId}/diagnostics within
// two seconds (the limit is this client's; the route is cheap on the host). An
// older one has no such route and is told by the version it reported on
// /api/server/capabilities, never by a 404, which 2.3.0 also gives for a
// connection it does not know or that is another device's; it is not asked at
// all. A Moonlight host has no route and is described from what its serverinfo
// already said.
// The decisions are here; the request is the network layer's.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dish::diagnostics {

inline constexpr int kSnapshotTimeoutMs = 2000;
inline constexpr std::string_view kConnectionSnapshotFormat = "satellite-connection-diagnostics.v1";
inline constexpr std::string_view kCapabilitiesSnapshotFormat = "satellite-capabilities";
inline constexpr std::string_view kMoonlightSnapshotFormat = "moonlight-serverinfo";

// `None` is a snapshot from the host itself; every other value means `snapshot` holds what this
// client already knew about the host, or nothing.
enum class SnapshotError : std::uint8_t {
    None,
    Offline,
    Unauthorized,
    OlderSatellite,
    Timeout,
    UnknownConnection,
    BadReply,
};

inline constexpr std::array<int, 3> kFirstSnapshotVersion{2, 3, 0};

// Below 2.3.0 by its own serverVersion. A version that does not read as numbers is not taken for an
// old one: the route is asked, and the answer says what it says. "2.3.0-rc.1" reads as 2.3.0.
inline bool isOlderSatellite(std::string_view serverVersion) {
    std::array<int, 3> parts{};
    std::size_t part = 0;
    bool sawDigit = false;
    for (const char ch : serverVersion) {
        if (ch >= '0' && ch <= '9') {
            parts[part] = parts[part] * 10 + (ch - '0');
            sawDigit = true;
            continue;
        }
        if (ch != '.' || part + 1 >= parts.size()) { break; }
        ++part;
    }
    if (!sawDigit) { return false; }
    return parts < kFirstSnapshotVersion;
}

struct SnapshotReply {
    // 0 when nothing answered.
    int status = 0;
    // The two-second deadline passed first.
    bool timedOut = false;
    // The body is a JSON object of kConnectionSnapshotFormat.
    bool bodyIsSnapshot = false;
};

// `Offline` is a host that never answered. One that answered with something unusable (a 5xx, or a
// 200 whose body is not a snapshot: a proxy, a page) is `BadReply`.
inline SnapshotError snapshotErrorOf(const SnapshotReply& reply) {
    constexpr int kNotFound = 404;
    constexpr int kUnauthorized = 401;
    constexpr int kForbidden = 403;
    if (reply.timedOut) { return SnapshotError::Timeout; }
    if (reply.status == kNotFound) { return SnapshotError::UnknownConnection; }
    if (reply.status == kUnauthorized || reply.status == kForbidden) {
        return SnapshotError::Unauthorized;
    }
    const bool answered = reply.status >= 200 && reply.status <= 299;
    if (answered && reply.bodyIsSnapshot) { return SnapshotError::None; }
    if (reply.status == 0) { return SnapshotError::Offline; }
    return SnapshotError::BadReply;
}

inline std::optional<std::string_view> snapshotErrorToken(SnapshotError error) {
    switch (error) {
    case SnapshotError::None:
        return std::nullopt;
    case SnapshotError::Offline:
        return "offline";
    case SnapshotError::Unauthorized:
        return "unauthorized";
    case SnapshotError::OlderSatellite:
        return "olderSatellite";
    case SnapshotError::Timeout:
        return "timeout";
    case SnapshotError::UnknownConnection:
        return "unknownConnection";
    case SnapshotError::BadReply:
        return "badReply";
    }
    return std::nullopt;
}

} // namespace dish::diagnostics
