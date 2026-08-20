// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The stable per-satellite identity key. Keying on the advertised machineId
// rather than the address means a box that takes a new DHCP lease keeps one
// remembered row, one pairing key and one cert pin. Satellites predating
// machineId fall back to "ip:udpPort".

#pragma once

#include <string>

namespace dish::model {

inline bool isBlank(const std::string& s) {
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') {
            return false;
        }
    }
    return true;
}

// A blank machineId counts as absent, so a beacon sending an empty "mid" keys on
// its address instead of collapsing every such box under one "mid:" key.
inline std::string stableKey(const std::string& machineId, const std::string& ip, int udpPort) {
    if (!isBlank(machineId)) { return "mid:" + machineId; }
    return ip + ":" + std::to_string(udpPort);
}

} // namespace dish::model
