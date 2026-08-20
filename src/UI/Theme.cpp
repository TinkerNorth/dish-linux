// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Theme.h"

#include <QGuiApplication>
#include <QStyleHints>

namespace dish::ui {

const ThemePalette& darkPalette() {
    static const ThemePalette kDark{
        /*background*/ 0xFF060818,
        /*surface*/ 0xFF0C1027,
        /*surfaceDim*/ 0xFF131A3A,
        /*primary*/ 0xFF4FE3FF,
        /*primaryDark*/ 0xFF2C93AD,
        /*onPrimary*/ 0xFF060818,
        /*onSurface*/ 0xFFE6ECFF,
        /*muted*/ 0xFF93A0C8,
        // outline is web rgba(79,227,255,0.18) expressed as ARGB (alpha 0x2E).
        /*outline*/ 0x2E4FE3FF,
        /*success*/ 0xFF22C55E,
        /*error*/ 0xFFE74C3C,
        /*warning*/ 0xFFF59E0B,
        /*pulse*/ 0xFFFF6FB5,
        /*glyph*/ 0xFF8FCFE3,       // 11.0:1 here, but 1.7:1 on white — hence a token
        /*disabledFg*/ 0xFF6C7799,  // 4.2:1 on surface
        /*mutedStrong*/ 0xFF7E8CB4, // 5.6:1 on surface
    };
    return kDark;
}

const ThemePalette& lightPalette() {
    // The cyan accent darkens here because a bright cyan on white is illegible;
    // status hues stay in family but shift to AA-contrast-on-light variants.
    static const ThemePalette kLight{
        /*background*/ 0xFFF5F7FC,
        /*surface*/ 0xFFFFFFFF,
        /*surfaceDim*/ 0xFFE7ECF6,
        /*primary*/ 0xFF0E7C97,
        /*primaryDark*/ 0xFF0A5E73,
        /*onPrimary*/ 0xFFFFFFFF,
        /*onSurface*/ 0xFF0C1430,
        /*muted*/ 0xFF5A6680,
        /*outline*/ 0x330E7C97, // darkened-cyan @ ~20% alpha
        /*success*/ 0xFF1B873F,
        /*error*/ 0xFFC0392B,
        /*warning*/ 0xFFB7791F,
        /*pulse*/ 0xFFC2417F,
        /*glyph*/ 0xFF2F7E96,
        // Deliberately LIGHTER than its dark counterpart: "disabled" reads as
        // receding, which on a white card means paler.
        /*disabledFg*/ 0xFF8A93A6,  // 3.1:1 on surface
        /*mutedStrong*/ 0xFF4B566E, // 7.4:1 on surface
    };
    return kLight;
}

const ThemePalette& paletteFor(Appearance appearance) {
    return appearance == Appearance::Light ? lightPalette() : darkPalette();
}

QRgb Theme::background = darkPalette().background;
QRgb Theme::surface = darkPalette().surface;
QRgb Theme::surfaceDim = darkPalette().surfaceDim;
QRgb Theme::primary = darkPalette().primary;
QRgb Theme::primaryDark = darkPalette().primaryDark;
QRgb Theme::onPrimary = darkPalette().onPrimary;
QRgb Theme::onSurface = darkPalette().onSurface;
QRgb Theme::muted = darkPalette().muted;
QRgb Theme::outline = darkPalette().outline;
QRgb Theme::success = darkPalette().success;
QRgb Theme::error = darkPalette().error;
QRgb Theme::warning = darkPalette().warning;
QRgb Theme::pulse = darkPalette().pulse;
QRgb Theme::glyph = darkPalette().glyph;
QRgb Theme::disabledFg = darkPalette().disabledFg;
QRgb Theme::mutedStrong = darkPalette().mutedStrong;

namespace {
Appearance g_activeAppearance = Appearance::Dark;
} // namespace

void setActivePalette(const ThemePalette& palette) {
    Theme::background = palette.background;
    Theme::surface = palette.surface;
    Theme::surfaceDim = palette.surfaceDim;
    Theme::primary = palette.primary;
    Theme::primaryDark = palette.primaryDark;
    Theme::onPrimary = palette.onPrimary;
    Theme::onSurface = palette.onSurface;
    Theme::muted = palette.muted;
    Theme::outline = palette.outline;
    Theme::success = palette.success;
    Theme::error = palette.error;
    Theme::warning = palette.warning;
    Theme::pulse = palette.pulse;
    Theme::glyph = palette.glyph;
    Theme::disabledFg = palette.disabledFg;
    Theme::mutedStrong = palette.mutedStrong;
}

Appearance activeAppearance() { return g_activeAppearance; }

void setActiveAppearance(Appearance appearance) {
    g_activeAppearance = appearance;
    setActivePalette(paletteFor(appearance));
}

// Qt reads the XDG org.freedesktop.appearance portal for this, so it is the
// whole implementation on Linux rather than a fallback. A desktop that exposes
// no portal reports Unknown and Dish stays dark.
Appearance detectSystemAppearance() {
    if (auto* hints = QGuiApplication::styleHints()) {
        if (hints->colorScheme() == Qt::ColorScheme::Light) { return Appearance::Light; }
        if (hints->colorScheme() == Qt::ColorScheme::Dark) { return Appearance::Dark; }
    }
    return Appearance::Dark;
}

QString hex(QRgb c) {
    return QStringLiteral("#%1%2%3")
        .arg(qRed(c), 2, 16, QLatin1Char('0'))
        .arg(qGreen(c), 2, 16, QLatin1Char('0'))
        .arg(qBlue(c), 2, 16, QLatin1Char('0'));
}

} // namespace dish::ui
