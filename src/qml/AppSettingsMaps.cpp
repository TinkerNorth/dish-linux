// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/AppSettingsMaps.h"

#include "Input/SDLGamepadBridge.h"
#include "core/model/Protocol.h"
#include "core/reducer/TouchpadModeResolve.h"
#include "repository/DeadzoneRepository.h"
#include "source/store/MotionEnabledStore.h"
#include "UI/licenses/LicenseManifest.h"

#include <QVariantMap>

#include <iterator>

namespace dish::qml {

namespace {

// The choices QML offers, each against the wire mode it is stored as. One table
// read both ways, so the pick a choice stores and the choice a pick reads as
// cannot drift apart. Off leads, because a mode outside the table reads as it,
// and the order is the binding draft's numbering: 0 off, 1 pad, 2 mouse.
struct TouchpadChoice {
    const char* token;
    std::uint8_t mode;
};

constexpr TouchpadChoice kTouchpadChoices[] = {
    {"off", proto::kTouchpadModeOff},
    {"pad", proto::kTouchpadModeDs4},
    {"mouse", proto::kTouchpadModeMouse},
};

} // namespace

QString touchpadChoiceForMode(std::uint8_t mode) {
    for (const auto& known : kTouchpadChoices) {
        if (known.mode == mode) { return QLatin1String(known.token); }
    }
    return QLatin1String(kTouchpadChoices[0].token);
}

int themeModeToInt(source::ThemeMode mode) {
    switch (mode) {
    case source::ThemeMode::Light:
        return 0;
    case source::ThemeMode::Dark:
        return 1;
    case source::ThemeMode::System:
        return 2;
    }
    return 2;
}

source::ThemeMode themeModeFromInt(int value) {
    switch (value) {
    case 0:
        return source::ThemeMode::Light;
    case 1:
        return source::ThemeMode::Dark;
    case 2:
    default:
        // Out-of-range is System too, matching themeModeFromStorage's lenient
        // unknown -> System default.
        return source::ThemeMode::System;
    }
}

int keepAwakeModeToInt(reducer::KeepAwakeMode mode) {
    switch (mode) {
    case reducer::KeepAwakeMode::Off:
        return 0;
    case reducer::KeepAwakeMode::WhileControllerActive:
        return 1;
    case reducer::KeepAwakeMode::WhileConnected:
        return 2;
    }
    return 1;
}

reducer::KeepAwakeMode keepAwakeModeFromInt(int value) {
    switch (value) {
    case 0:
        return reducer::KeepAwakeMode::Off;
    case 2:
        return reducer::KeepAwakeMode::WhileConnected;
    case 1:
    default:
        // Out-of-range lands on the timed mode, matching keepAwakeModeFromKey:
        // a bad value must never pin the machine awake.
        return reducer::KeepAwakeMode::WhileControllerActive;
    }
}

QString keepAwakeReachToken(reducer::KeepAwakeReach reach) {
    switch (reach) {
    case reducer::KeepAwakeReach::System:
        return QStringLiteral("system");
    case reducer::KeepAwakeReach::SystemAndDisplay:
        return QStringLiteral("display");
    case reducer::KeepAwakeReach::None:
        break;
    }
    return QStringLiteral("off");
}

QString touchpadChoiceForPick(const std::optional<std::string>& pick, bool mouseModeAvailable) {
    const std::uint8_t mode = proto::touchpadModeFromName(reducer::touchpadPickOrDefault(pick));
    const bool mouseShut = mode == proto::kTouchpadModeMouse && !mouseModeAvailable;
    return touchpadChoiceForMode(mouseShut ? proto::kTouchpadModeOff : mode);
}

std::optional<QString> touchpadChoiceForDraftMode(int draftMode) {
    constexpr int kChoiceCount = static_cast<int>(std::size(kTouchpadChoices));
    const bool isAChoice = draftMode >= 0 && draftMode < kChoiceCount;
    if (!isAChoice) { return std::nullopt; }
    return QLatin1String(kTouchpadChoices[static_cast<std::size_t>(draftMode)].token);
}

std::optional<std::string> touchpadPickForChoice(const QString& choice) {
    for (const auto& known : kTouchpadChoices) {
        if (choice == QLatin1String(known.token)) {
            return std::string(proto::touchpadModeName(known.mode));
        }
    }
    return std::nullopt;
}

QVariantMap deadzoneRowFor(const QString& deviceId, const QString& name, bool hasGyro,
                           const dish::repository::DeadzoneRepository* deadzoneRepo,
                           const dish::source::MotionEnabledStore* motionStore) {
    int stickFlat = kDefaultDeadzoneStickFlat;
    int triggerFlat = kDefaultDeadzoneTriggerFlat;
    if (deadzoneRepo != nullptr) {
        if (auto stored = deadzoneRepo->deadzonesFor(deviceId)) {
            stickFlat = stored->stickFlat;
            triggerFlat = stored->triggerFlat;
        }
    }
    // Surfaced whether or not the pad has a gyro; the page owns the toggle's
    // visibility off hasGyro.
    bool forwardMotion = source::MotionEnabledStore::kDefaultEnabled;
    if (motionStore != nullptr) { forwardMotion = motionStore->isEnabled(deviceId.toStdString()); }

    QVariantMap m;
    m[QStringLiteral("id")] = deviceId;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("hasGyro")] = hasGyro;
    m[QStringLiteral("stickFlat")] = stickFlat;
    m[QStringLiteral("triggerFlat")] = triggerFlat;
    m[QStringLiteral("forwardMotion")] = forwardMotion;
    return m;
}

QVariantList deadzoneDeviceRows(const dish::input::SDLGamepadBridge* bridge,
                                const dish::repository::DeadzoneRepository* deadzoneRepo,
                                const dish::source::MotionEnabledStore* motionStore) {
    QVariantList out;
    if (bridge == nullptr) { return out; }
    for (const auto& d : bridge->devices()) {
        out.append(deadzoneRowFor(d.id, d.name, d.motionCapable, deadzoneRepo, motionStore));
    }
    return out;
}

QVariantList licenseRows(const dish::ui::LicenseManifest& manifest) {
    QVariantList out;
    for (const auto& entry : manifest.libraries) {
        const QString name = dish::ui::licenseDisplayName(entry);
        if (name.isEmpty()) { continue; }
        QVariantMap m;
        m[QStringLiteral("name")] = name;
        m[QStringLiteral("version")] = dish::ui::licenseVersionLabel(entry);
        const auto label = dish::ui::licenseLabel(entry);
        m[QStringLiteral("license")] = label.has_value() ? *label : QString();
        const auto url = dish::ui::licenseClickUrl(entry);
        m[QStringLiteral("url")] = url.has_value() ? *url : QString();
        out.append(m);
    }
    return out;
}

} // namespace dish::qml
