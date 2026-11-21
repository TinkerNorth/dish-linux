// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// `bluetooth.transportLayer` for a pad SDL opened (docs/diagnostics-bundle.md, "Every controller
// path"): a HIDP pad is classic from sysfs alone; a uhid pad is asked of BlueZ, whose
// org.bluez.Device1.UUIDs for the address in its HID_UNIQ names the HID service it was made for.
// The Flatpak may ask: its manifest holds --system-talk-name=org.bluez. The address only builds
// BlueZ's object path inside readTransportLayer and is gone when it returns.

#pragma once

#include "core/input/BluetoothServices.h"
#include "core/input/HidTransport.h"

#include <optional>
#include <string>
#include <vector>

namespace dish::source {

// BlueZ's service list for the device at `address` ("AA:BB:CC:DD:EE:FF") on any adapter sysfs
// lists; none when the system bus or BlueZ cannot be reached, or no adapter knows the device.
std::optional<std::vector<std::string>> bluezServiceUuids(const std::string& address);

// The link of the pad behind `hidNode`, which the SDL bridge classified at attach.
input::TransportLayer readTransportLayer(input::BluetoothHidOrigin origin,
                                         const std::string& hidNode);

} // namespace dish::source
