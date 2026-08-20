// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SettingsKeys — the QSettings key namespaces the trust/identity repositories
// co-tenant in one connection-store file (HKCU\Software\Dish\Dish).
//
// Namespace isolation is a hard invariant: a cert pin must never read back as a
// shared key, and clear() on one repo must not touch another's keys. These
// prefixes are the only thing keeping them disjoint, which is why they live in
// one place rather than inline in each repo.

#pragma once

namespace dish::repository::keys {

// Per-id cert-pin entries: "satellite_cert_pin:<id>" -> fingerprint hex.
inline constexpr const char* kCertPinPrefix = "satellite_cert_pin:";

// Per-id pairing-key entries: "satellite_shared_key:<id>" -> key hex. Distinct
// from the legacy prefix below, which ConnectionStore migrates on first read.
inline constexpr const char* kSharedKeyPrefix = "satellite_shared_key:";

// The remembered-satellite list, one JSON array under a single key.
inline constexpr const char* kSatelliteListKey = "satellite_list";

// Per-device deadzone profiles: "deadzone:<deviceId>" -> {s,t} JSON.
inline constexpr const char* kDeadzonePrefix = "deadzone:";

// Per-slot motion-enable toggles: "motion_enabled:<slotId>" -> "1"/"0".
// Absence means "never written" -> the store defaults it on.
inline constexpr const char* kMotionEnabledPrefix = "motion_enabled:";

// Stable per-install device id (machineId source for X-Device-Id). One owner.
inline constexpr const char* kDeviceIdKey = "deviceId";

// Retired key names, kept only so old installs can be upgraded in place.
inline constexpr const char* kLegacyWifiListKey = "wifi_list";
inline constexpr const char* kLegacySharedKeyPrefix = "wifi_shared_key/";

} // namespace dish::repository::keys
