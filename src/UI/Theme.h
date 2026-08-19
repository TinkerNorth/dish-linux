// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Colour tokens, lifted verbatim from dish-android's colors.xml so every client
// renders identically side-by-side. Both palettes MUST stay in lockstep with the
// android `values/` and `values-night/` resources; DESIGN.md carries the token
// <-> hex mapping and the cross-client schema the names come from.

#pragma once

#include <QColor>
#include <QString>

namespace dish::ui {

struct ThemePalette {
    QRgb background;
    QRgb surface;
    QRgb surfaceDim;
    QRgb primary;
    QRgb primaryDark;
    QRgb onPrimary;
    QRgb onSurface;
    QRgb muted;
    QRgb outline;
    QRgb success;
    QRgb error;
    QRgb warning;
    // The donation accent — the one hue Dish uses beyond cyan, reserved for the
    // Support Dish surface and its rail entry.
    QRgb pulse;
    // The shipped SVGs bake a light-cyan that computes to 1.7:1 on a white card,
    // so BrandGlyph re-tints by palette (never by state).
    QRgb glyph;
    // Disabled-control foreground, >= 3:1 on `surface` in both palettes. Never
    // multiplied by an opacity on top of an already-muted colour.
    QRgb disabledFg;
    // Drawn-but-unavailable INFORMATION, >= 4.5:1. Distinct from `disabledFg`,
    // which is for controls.
    QRgb mutedStrong;
};

// The resolved half of android's ThemeMode: System is resolved to one of these
// before a palette is selected.
enum class Appearance { Dark, Light };

const ThemePalette& darkPalette();
const ThemePalette& lightPalette();
const ThemePalette& paletteFor(Appearance appearance);

struct Theme {
    // Non-const statics aliasing the ACTIVE palette's fields, so call sites
    // resolve to the currently-applied appearance. setActivePalette() rewrites
    // them; they start on dark so a build that never calls it still renders.
    static QRgb background;
    static QRgb surface;
    static QRgb surfaceDim;
    static QRgb primary;
    static QRgb primaryDark;
    static QRgb onPrimary;
    static QRgb onSurface;
    static QRgb muted;
    static QRgb outline;
    static QRgb success;
    static QRgb error;
    static QRgb warning;
    static QRgb pulse;
    static QRgb glyph;
    static QRgb disabledFg;
    static QRgb mutedStrong;
};

// Swaps the Theme::* tokens. Callers must re-read them: the QML ThemeBridge
// does so on refresh(), and the native chrome flips its immersive-dark attribute.
void setActivePalette(const ThemePalette& palette);

Appearance activeAppearance();
void setActiveAppearance(Appearance appearance);

// The OS appearance-preference seam. Injected as a std::function in tests so
// System resolution can be driven without touching the real registry.
Appearance detectSystemAppearance();

QString hex(QRgb c);

} // namespace dish::ui
