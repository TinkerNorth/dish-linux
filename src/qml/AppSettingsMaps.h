// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure mapping helpers behind AppViewModel's settings/deadzone/license
// invokables. Split out so they unit-test without constructing AppModel, which
// owns SDL/USB/timers.

#pragma once

#include "core/reducer/KeepAwake.h"
#include "source/store/ThemePreferenceStore.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>

namespace dish::ui {
struct LicenseManifest;
}

namespace dish::input {
class SDLGamepadBridge;
}

namespace dish::repository {
class DeadzoneRepository;
}

namespace dish::source {
class MotionEnabledStore;
}

namespace dish::qml {

// The contract order 0=Light, 1=Dark, 2=System deliberately differs from the
// enum's declaration order, so this is a real mapping and not a static_cast.
int themeModeToInt(source::ThemeMode mode);
source::ThemeMode themeModeFromInt(int value);

// The contract order 0=Off, 1=WhileControllerActive, 2=WhileConnected — the
// SettingsPage option order, and again a real mapping, not a static_cast.
int keepAwakeModeToInt(reducer::KeepAwakeMode mode);
reducer::KeepAwakeMode keepAwakeModeFromInt(int value);

// "off" | "system" | "display": how far the hold currently reaches.
QString keepAwakeReachToken(reducer::KeepAwakeReach reach);

// The profile the SDL bridge installs at attach; a row with no stored override
// seeds from it, so the two must not drift.
constexpr int kDefaultDeadzoneStickFlat = 3277;
constexpr int kDefaultDeadzoneTriggerFlat = 13;

// { id, name, hasGyro, stickFlat, triggerFlat, forwardMotion }. Both stores are
// borrowed and may be null (-> defaults).
QVariantMap deadzoneRowFor(const QString& deviceId, const QString& name, bool hasGyro,
                           const dish::repository::DeadzoneRepository* deadzoneRepo,
                           const dish::source::MotionEnabledStore* motionStore);

// One row per attached SDL device. `bridge` may be null -> empty list.
QVariantList deadzoneDeviceRows(const dish::input::SDLGamepadBridge* bridge,
                                const dish::repository::DeadzoneRepository* deadzoneRepo,
                                const dish::source::MotionEnabledStore* motionStore);

// { name, version, license, url }; an entry with no display name is dropped.
QVariantList licenseRows(const dish::ui::LicenseManifest& manifest);

} // namespace dish::qml
