// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The PC's power and thermal files, read when the support report is prepared:
// every /sys/class/power_supply entry, /sys/firmware/acpi/platform_profile and
// every /sys/class/thermal zone with its trip points, which explain a latency
// or rumble complaint on a handheld. What they mean is
// core/diagnostics/DeviceFacts.h's to decide. Each root is a parameter so a
// test can point it at a folder of its own.

#pragma once

#include "core/diagnostics/DeviceFacts.h"

#include <QString>

#include <optional>

namespace dish::source {

inline constexpr const char* kPowerSupplyRoot = "/sys/class/power_supply";
inline constexpr const char* kPlatformProfilePath = "/sys/firmware/acpi/platform_profile";
inline constexpr const char* kThermalRoot = "/sys/class/thermal";

// The system battery and the power profile; each part empty where its file is not there.
diagnostics::PowerFacts
readPowerFacts(const QString& powerSupplyRoot = QLatin1String(kPowerSupplyRoot),
               const QString& platformProfilePath = QLatin1String(kPlatformProfilePath));

// Empty where no thermal zone has a temperature and a trip point to judge it by.
std::optional<diagnostics::ThermalState>
readThermalState(const QString& thermalRoot = QLatin1String(kThermalRoot));

} // namespace dish::source
