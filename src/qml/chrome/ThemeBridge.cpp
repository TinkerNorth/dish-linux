// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/chrome/ThemeBridge.h"

#include "UI/Theme.h"

namespace dish::chrome {

namespace {

// The washes are derived from the live accent rather than copied out of the
// token sheet as rgba literals, so a palette swap has no second table to miss.
QColor withAlpha(QRgb base, int alpha) {
    QColor c = QColor::fromRgba(base);
    c.setAlpha(alpha);
    return c;
}

bool lightActive() { return dish::ui::activeAppearance() == dish::ui::Appearance::Light; }

} // namespace

ThemeBridge::ThemeBridge(QObject* parent) : QObject(parent) {}

QColor ThemeBridge::background() const { return QColor::fromRgba(dish::ui::Theme::background); }
QColor ThemeBridge::surface() const { return QColor::fromRgba(dish::ui::Theme::surface); }
QColor ThemeBridge::surfaceDim() const { return QColor::fromRgba(dish::ui::Theme::surfaceDim); }
QColor ThemeBridge::primary() const { return QColor::fromRgba(dish::ui::Theme::primary); }
QColor ThemeBridge::primaryDark() const { return QColor::fromRgba(dish::ui::Theme::primaryDark); }
QColor ThemeBridge::onPrimary() const { return QColor::fromRgba(dish::ui::Theme::onPrimary); }
QColor ThemeBridge::onSurface() const { return QColor::fromRgba(dish::ui::Theme::onSurface); }
QColor ThemeBridge::muted() const { return QColor::fromRgba(dish::ui::Theme::muted); }
QColor ThemeBridge::outline() const { return QColor::fromRgba(dish::ui::Theme::outline); }
QColor ThemeBridge::success() const { return QColor::fromRgba(dish::ui::Theme::success); }
QColor ThemeBridge::error() const { return QColor::fromRgba(dish::ui::Theme::error); }
QColor ThemeBridge::warning() const { return QColor::fromRgba(dish::ui::Theme::warning); }
QColor ThemeBridge::glyph() const { return QColor::fromRgba(dish::ui::Theme::glyph); }
QColor ThemeBridge::disabledFg() const { return QColor::fromRgba(dish::ui::Theme::disabledFg); }
QColor ThemeBridge::mutedStrong() const { return QColor::fromRgba(dish::ui::Theme::mutedStrong); }

// tokens/colors.css: dark hover/press/fill = 12/18/14 %, light = 10/16/12 %;
// warning fill is 16 % in both.
QColor ThemeBridge::primaryHover() const {
    return withAlpha(dish::ui::Theme::primary, lightActive() ? 26 : 31);
}
QColor ThemeBridge::primaryPress() const {
    return withAlpha(dish::ui::Theme::primary, lightActive() ? 41 : 46);
}
QColor ThemeBridge::primaryFill() const {
    return withAlpha(dish::ui::Theme::primary, lightActive() ? 31 : 36);
}
QColor ThemeBridge::warningFill() const { return withAlpha(dish::ui::Theme::warning, 41); }

// The 24 % step above primaryHover (12 %) and primaryPress (18 %).
QColor ThemeBridge::accentWash24() const {
    return withAlpha(dish::ui::Theme::primary, lightActive() ? 56 : 61);
}
QColor ThemeBridge::successFill() const {
    return withAlpha(dish::ui::Theme::success, lightActive() ? 31 : 36);
}
QColor ThemeBridge::errorFill() const { return withAlpha(dish::ui::Theme::error, 41); }

// Body colour at 60 %, so the scrim darkens on dark and lightens on light.
QColor ThemeBridge::scrim() const { return withAlpha(dish::ui::Theme::background, 153); }
// Accent at 30 %, drawn outside the 1 px accent border.
QColor ThemeBridge::focusRing() const { return withAlpha(dish::ui::Theme::primary, 77); }
// Accent at 9 %: a full `outline` on an intra-card divider reads as a second
// card edge.
QColor ThemeBridge::outlineSubtle() const { return withAlpha(dish::ui::Theme::primary, 23); }

// Donation accent, fill 12 % and edge 35 %; both alphas are appearance-invariant.
QColor ThemeBridge::pulse() const { return QColor::fromRgba(dish::ui::Theme::pulse); }
QColor ThemeBridge::pulseFill() const { return withAlpha(dish::ui::Theme::pulse, 31); }
QColor ThemeBridge::pulseEdge() const { return withAlpha(dish::ui::Theme::pulse, 89); }

void ThemeBridge::refresh() { emit paletteChanged(); }

QColor ThemeBridge::alpha(const QColor& c, qreal a) const {
    QColor out = c;
    out.setAlphaF(static_cast<float>(qBound(0.0, a, 1.0)));
    return out;
}

} // namespace dish::chrome
