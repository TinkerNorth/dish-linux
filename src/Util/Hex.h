// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::util {

// Encode raw bytes as lowercase hex.
std::string toHex(const std::uint8_t* data, std::size_t size);
std::string toHex(const std::vector<std::uint8_t>& bytes);

// Decode an even-length lowercase/uppercase hex string. Returns std::nullopt on
// invalid input (odd length or non-hex characters).
std::optional<std::vector<std::uint8_t>> fromHex(std::string_view hex);

} // namespace dish::util
