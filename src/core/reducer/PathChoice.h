// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A user's explicit per-model USB input-path override: Direct claims the raw HID
// device, Standard lets SDL own the pad. Auto is the ABSENCE of a stored
// value (std::nullopt), never a third enum value, so resolvePathChoice in
// UsbPathMachine.h can pick per model.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dish::reducer {

enum class PathChoice {
    Direct,
    Standard,
};

// Persisted verbatim in the per-VID:PID QSettings store. Add values, never
// rename an existing one.
inline constexpr std::string_view kPathChoiceStorageDirect = "direct";
inline constexpr std::string_view kPathChoiceStorageStandard = "standard";

inline std::string toStorageValue(PathChoice choice) {
    switch (choice) {
    case PathChoice::Direct:
        return std::string(kPathChoiceStorageDirect);
    case PathChoice::Standard:
        return std::string(kPathChoiceStorageStandard);
    }
    return std::string(kPathChoiceStorageStandard); // unreachable: the switch is total
}

// An unrecognised value (one a newer build wrote) resolves to Auto rather than
// failing, for forward-compat.
inline std::optional<PathChoice> pathChoiceFromStorageValue(std::string_view value) {
    if (value == kPathChoiceStorageDirect) { return PathChoice::Direct; }
    if (value == kPathChoiceStorageStandard) { return PathChoice::Standard; }
    return std::nullopt;
}

} // namespace dish::reducer
