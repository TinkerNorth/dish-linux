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

// Lowercase output.
std::string toHex(const std::uint8_t* data, std::size_t size);
std::string toHex(const std::vector<std::uint8_t>& bytes);

// Either case accepted; nullopt on odd length or a non-hex character.
std::optional<std::vector<std::uint8_t>> fromHex(std::string_view hex);

} // namespace dish::util
