// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Trust-On-First-Use cert pinning. The satellite presents a self-signed cert, so
// there is no chain to validate: pin the SHA-256 of the cert first seen for an id
// and reject any later cert that differs. Storage lives in
// repository/SatellitePinRepository, the TLS callback in source/http.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace dish::net {

enum class TofuVerdict {
    TrustFirstUse, // no pin stored yet: accept and pin
    Match,         // presented equals stored (case-insensitive hex)
    Mismatch,      // a pin exists and differs: reject, keep the pin
};

// Only std::nullopt means never-pinned. An empty-string stored pin is a present,
// non-matching pin and therefore Mismatches.
TofuVerdict tofuVerdict(const std::optional<std::string>& stored, const std::string& presented);

// Lowercase 64-char hex SHA-256 of `bytes` (the cert's DER encoding).
std::string sha256FingerprintHex(const std::uint8_t* bytes, std::size_t len);

} // namespace dish::net
