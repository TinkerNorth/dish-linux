// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Color palette lifted verbatim from dish-android/res/values/colors.xml so
// every client renders identically side-by-side. Same hex values as
// dish-mac/UI/Theme.swift.

#pragma once

#include <QApplication>
#include <QColor>
#include <QString>

namespace dish::ui {

struct Theme {
    // Cyan / deep-space palette — mirrors dish-website. See DESIGN.md for
    // the token name <-> hex mapping and the cross-repo schema.
    static constexpr QRgb background = 0xFF060818;
    static constexpr QRgb surface = 0xFF0C1027;
    static constexpr QRgb surfaceDim = 0xFF131A3A;
    static constexpr QRgb primary = 0xFF4FE3FF;
    static constexpr QRgb primaryDark = 0xFF2C93AD;
    static constexpr QRgb onPrimary = 0xFF060818;
    static constexpr QRgb onSurface = 0xFFE6ECFF;
    static constexpr QRgb muted = 0xFF93A0C8;
    // outline is web rgba(79,227,255,0.18) expressed as ARGB (alpha 0x2E).
    static constexpr QRgb outline = 0x2E4FE3FF;
    static constexpr QRgb success = 0xFF22C55E;
    static constexpr QRgb error = 0xFFE74C3C;
    static constexpr QRgb warning = 0xFFF59E0B;
};

// Apply the global Qt palette + a stylesheet matching dish-android's themes.
void applyDishTheme(QApplication& app);

// Format a QRgb as a `#RRGGBB` string for embedding in QSS.
QString hex(QRgb c);

// Style helpers used by the dialogs / SlotCard.
QString sectionHeaderQss();
QString outlinedButtonQss();
QString dotQss(QRgb color);

} // namespace dish::ui
