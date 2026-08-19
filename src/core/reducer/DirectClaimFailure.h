// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Why a Direct-mode (raw-HID claim) attempt could not be completed. Surfaced on
// the controller card so the user sees the real cause rather than a blanket
// "unplug and replug".

#pragma once

namespace dish::reducer {

enum class DirectClaimFailure {
    PermissionDenied, // ERROR_ACCESS_DENIED on CreateFile, or a privileged owner
    Busy,             // ERROR_SHARING_VIOLATION, or WinUSB initialize refused
    InitFailed,       // claimed, but no decodable report ever arrived
    Dropped,          // the OS detached the device on claim; needs a physical replug
};

} // namespace dish::reducer
