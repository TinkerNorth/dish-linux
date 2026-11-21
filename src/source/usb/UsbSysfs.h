// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What sysfs says about a USB pad and the machine's USB hosts, read without privilege
// (Documentation/ABI/stable/sysfs-bus-usb): the device's `descriptors` (the raw device
// descriptor, then every configuration's), its `bcdDevice`, `manufacturer` and `product`, and
// the root hubs `usbN` under /sys/bus/usb/devices, one per host controller's bus. The serial
// number identifies one unit and is never opened. Flatpak shares /sys/bus, /sys/class and
// /sys/devices read-only with every sandbox (flatpak-run.c, sysfs_dirs), so the readers see the
// same tree inside it.

#pragma once

#include "core/diagnostics/UsbDescriptors.h"

#include <optional>
#include <string>

namespace dish::source::usb {

// The pad's hardware from its USB device directory, with the claimed interface's report
// descriptor as the gateway read it, the system's copy. Each fact a file could not give is left
// empty.
diagnostics::UsbHardwareFacts readUsbHardware(const std::string& usbDeviceDir, int interfaceNumber,
                                              diagnostics::DescriptorBytes reportDescriptor);

// The root hubs under `devicesDir` ("/sys/bus/usb/devices" outside a test); none when the
// directory cannot be read.
std::optional<int> countRootHubs(const std::string& devicesDir = "/sys/bus/usb/devices");

} // namespace dish::source::usb
