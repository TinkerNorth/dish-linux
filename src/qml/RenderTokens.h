// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Render-key -> QML token-string mappers. One switch per enum, shared by every
// surface that vends these tokens, so the vocabulary cannot drift; QML
// localizes and colours FROM the tokens and never re-derives them.

#pragma once

#include "core/reducer/ConnectionRows.h"
#include "core/reducer/SatelliteLinkState.h"

#include <QString>

namespace dish::qml::tokens {

inline QString linkStateToken(reducer::UiLinkState s) {
    switch (s) {
    case reducer::UiLinkState::Found:
        return QStringLiteral("found");
    case reducer::UiLinkState::Stale:
        return QStringLiteral("stale");
    case reducer::UiLinkState::Saved:
        return QStringLiteral("saved");
    case reducer::UiLinkState::Ready:
        return QStringLiteral("ready");
    case reducer::UiLinkState::Connecting:
        return QStringLiteral("connecting");
    case reducer::UiLinkState::Connected:
        return QStringLiteral("connected");
    case reducer::UiLinkState::Unstable:
        return QStringLiteral("unstable");
    }
    return {};
}

inline QString chipToken(reducer::StatusChipKey c) {
    switch (c) {
    case reducer::StatusChipKey::Found:
        return QStringLiteral("found");
    case reducer::StatusChipKey::NeedsPairing:
        return QStringLiteral("needsPairing");
    case reducer::StatusChipKey::Offline:
        return QStringLiteral("offline");
    case reducer::StatusChipKey::Ready:
        return QStringLiteral("ready");
    case reducer::StatusChipKey::Connecting:
        return QStringLiteral("connecting");
    case reducer::StatusChipKey::Online:
        return QStringLiteral("online");
    case reducer::StatusChipKey::Unstable:
        return QStringLiteral("unstable");
    }
    return {};
}

inline QString dotToken(reducer::DotColor d) {
    switch (d) {
    case reducer::DotColor::Success:
        return QStringLiteral("success");
    case reducer::DotColor::Primary:
        return QStringLiteral("primary");
    case reducer::DotColor::Warning:
        return QStringLiteral("warning");
    case reducer::DotColor::Muted:
        return QStringLiteral("muted");
    }
    return {};
}

inline QString glyphToken(reducer::ConnectionGlyph g) {
    switch (g) {
    case reducer::ConnectionGlyph::SatelliteBase:
        return QStringLiteral("satelliteBase");
    case reducer::ConnectionGlyph::SatelliteConnected:
        return QStringLiteral("satelliteConnected");
    case reducer::ConnectionGlyph::SatelliteOff:
        return QStringLiteral("satelliteOff");
    }
    return {};
}

} // namespace dish::qml::tokens
