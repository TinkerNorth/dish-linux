// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the diagnostics bundle and the issue match know about this PC: its
// model (the firmware's vendor and product strings, which the kernel publishes
// world-readable as /sys/class/dmi/id/sys_vendor and product_name), the
// distribution and kernel release, and the CPU architecture. Read once;
// nothing here identifies the person or the machine beyond its model, and the
// host name is never read. A machine whose firmware publishes no DMI (most
// ARM boards) or a sandbox that hides it is "Linux PC".

#pragma once

#include <QString>

namespace dish::source {

struct MachineIdentity {
    QString model;       // "AYN Thor", core/diagnostics/HostIdentity.h's machineModelOf
    QString os;          // "Ubuntu 24.04.1 LTS (6.8.0-45-generic)"
    QString arch;        // "x86_64"
    int kernelMajor = 0; // 6
};

inline constexpr const char* kDmiDir = "/sys/class/dmi/id";

// `dmiDir` is the folder sys_vendor and product_name are read from; a test points it elsewhere.
MachineIdentity readMachineIdentity(const QString& dmiDir = QString::fromLatin1(kDmiDir));

} // namespace dish::source
