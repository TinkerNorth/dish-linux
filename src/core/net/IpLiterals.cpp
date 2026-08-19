// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/net/IpLiterals.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace dish::net {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isHexDigit(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Keeps empties: the IPv6 grammar below relies on "a::b" splitting to
// ["a","","b"] rather than collapsing.
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::optional<std::array<int, 4>> parseIpv4(const std::string& host) {
    const auto parts = split(host, '.');
    if (parts.size() != 4) { return std::nullopt; }
    std::array<int, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::string& p = parts[i];
        if (p.empty() || p.size() > 3) { return std::nullopt; }
        int value = 0;
        for (const char c : p) {
            if (!isDigit(c)) { return std::nullopt; }
            value = value * 10 + (c - '0');
        }
        if (value > 255) { return std::nullopt; }
        out[i] = value;
    }
    return out;
}

bool isPrivateIpv4(const std::array<int, 4>& o) {
    if (o[0] == 10) { return true; }                              // 10.0.0.0/8
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) { return true; } // 172.16.0.0/12
    if (o[0] == 192 && o[1] == 168) { return true; }              // 192.168.0.0/16
    if (o[0] == 169 && o[1] == 254) { return true; }              // 169.254.0.0/16
    if (o[0] == 127) { return true; }                             // 127.0.0.0/8
    return false;
}

// Handles one "::" compression and an optional embedded IPv4 in the final 32
// bits. Zone ids are rejected rather than stripped.
std::optional<std::array<std::uint8_t, 16>> parseIpv6(const std::string& host) {
    if (host.find('%') != std::string::npos) { return std::nullopt; } // zone id unsupported

    std::string head;
    std::string tail;
    bool hasCompression = false;
    {
        const auto pos = host.find("::");
        if (pos != std::string::npos) {
            hasCompression = true;
            head = host.substr(0, pos);
            tail = host.substr(pos + 2);
            if (tail.find("::") != std::string::npos) { return std::nullopt; }
        } else {
            head = host;
        }
    }

    // A colon-separated list of hextets; the last may be an embedded IPv4.
    const auto groupsOf = [](const std::string& part,
                             bool allowEmbeddedV4) -> std::optional<std::vector<std::uint16_t>> {
        std::vector<std::uint16_t> groups;
        if (part.empty()) { return groups; }
        const auto pieces = split(part, ':');
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const std::string& piece = pieces[i];
            const bool last = (i + 1 == pieces.size());
            if (last && allowEmbeddedV4 && piece.find('.') != std::string::npos) {
                const auto v4 = parseIpv4(piece);
                if (!v4.has_value()) { return std::nullopt; }
                groups.push_back(static_cast<std::uint16_t>(((*v4)[0] << 8) | (*v4)[1]));
                groups.push_back(static_cast<std::uint16_t>(((*v4)[2] << 8) | (*v4)[3]));
                continue;
            }
            if (piece.empty() || piece.size() > 4) { return std::nullopt; }
            int value = 0;
            for (const char c : piece) {
                if (!isHexDigit(c)) { return std::nullopt; }
                const int digit = isDigit(c)   ? (c - '0')
                                  : (c >= 'a') ? (c - 'a' + 10)
                                               : (c - 'A' + 10);
                value = value * 16 + digit;
            }
            groups.push_back(static_cast<std::uint16_t>(value));
        }
        return groups;
    };

    const auto headGroups = groupsOf(head, !hasCompression);
    if (!headGroups.has_value()) { return std::nullopt; }
    const auto tailGroups = groupsOf(tail, true);
    if (!tailGroups.has_value()) { return std::nullopt; }

    std::vector<std::uint16_t> full;
    if (hasCompression) {
        const std::size_t present = headGroups->size() + tailGroups->size();
        if (present >= 8) { return std::nullopt; } // "::" must stand for ≥1 group
        full.insert(full.end(), headGroups->begin(), headGroups->end());
        full.insert(full.end(), 8 - present, 0);
        full.insert(full.end(), tailGroups->begin(), tailGroups->end());
    } else {
        if (headGroups->size() != 8) { return std::nullopt; }
        full = *headGroups;
    }
    if (full.size() != 8) { return std::nullopt; }

    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i * 2] = static_cast<std::uint8_t>(full[i] >> 8);
        bytes[i * 2 + 1] = static_cast<std::uint8_t>(full[i] & 0xFF);
    }
    return bytes;
}

bool isPrivateIpv6(const std::array<std::uint8_t, 16>& b) {
    // ::1 loopback
    bool allZeroToFourteen = true;
    for (std::size_t i = 0; i < 15; ++i) {
        if (b[i] != 0) {
            allZeroToFourteen = false;
            break;
        }
    }
    if (allZeroToFourteen && b[15] == 1) { return true; }
    if (b[0] == 0xfc || b[0] == 0xfd) { return true; }          // fc00::/7 unique-local
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) { return true; } // fe80::/10 link-local
    return false;
}

} // namespace

bool isPrivateHostLiteral(const std::string& host) {
    // Strip the IPv6 URL-authority brackets, e.g. "[fe80::1]".
    std::string h = host;
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') { h = h.substr(1, h.size() - 2); }
    if (const auto v4 = parseIpv4(h)) { return isPrivateIpv4(*v4); }
    if (const auto v6 = parseIpv6(h)) { return isPrivateIpv6(*v6); }
    return false; // not a literal -> not private
}

} // namespace dish::net
