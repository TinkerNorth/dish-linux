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
    static constexpr QRgb background  = 0xFF0D0F12;
    static constexpr QRgb surface     = 0xFF161A1F;
    static constexpr QRgb surfaceDim  = 0xFF111417;
    static constexpr QRgb primary     = 0xFFFFC107;  // amber
    static constexpr QRgb primaryDark = 0xFFA65F1E;
    static constexpr QRgb onPrimary   = 0xFF0D0F12;
    static constexpr QRgb onSurface   = 0xFFEAEAEA;
    static constexpr QRgb muted       = 0xFF6B7280;
    static constexpr QRgb outline     = 0xFF222831;
    static constexpr QRgb success     = 0xFF22C55E;
    static constexpr QRgb error       = 0xFFE74C3C;
    static constexpr QRgb warning     = 0xFFF59E0B;
};

// Apply the global Qt palette + a stylesheet matching dish-android's themes.
void applyDishTheme(QApplication& app);

// Style helpers used by the dialogs / SlotCard.
QString sectionHeaderQss();
QString outlinedButtonQss();
QString dotQss(QRgb color);

}  // namespace dish::ui
