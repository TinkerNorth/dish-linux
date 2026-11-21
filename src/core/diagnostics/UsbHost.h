// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// `device.usbHost` (docs/diagnostics-bundle.md, "Device facts"): measured, never assumed. Each
// platform's reader counts the host controllers its system reports (Linux its root hubs under
// /sys/bus/usb/devices, Windows the present interfaces of GUID_DEVINTERFACE_USB_HOST_CONTROLLER)
// and hands the count here; a reader the system gave no answer hands none.

#pragma once

#include <optional>

namespace dish::diagnostics {

inline std::optional<bool> usbHostOf(std::optional<int> hostControllers) {
    if (!hostControllers.has_value()) { return std::nullopt; }
    return *hostControllers > 0;
}

} // namespace dish::diagnostics
