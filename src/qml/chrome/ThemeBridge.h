// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Exposes the C++ design-token palette (dish::ui::Theme) to QML as a `Theme`
// singleton.

#pragma once

#include <QColor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace dish::chrome {

class ThemeBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Theme)
    QML_SINGLETON
    // NOTIFY, not CONSTANT: setThemeMode swaps the active palette live, so every
    // token has to re-read. refresh() is what re-emits.
    Q_PROPERTY(QColor background READ background NOTIFY paletteChanged)
    Q_PROPERTY(QColor surface READ surface NOTIFY paletteChanged)
    Q_PROPERTY(QColor surfaceDim READ surfaceDim NOTIFY paletteChanged)
    Q_PROPERTY(QColor primary READ primary NOTIFY paletteChanged)
    Q_PROPERTY(QColor primaryDark READ primaryDark NOTIFY paletteChanged)
    Q_PROPERTY(QColor onPrimary READ onPrimary NOTIFY paletteChanged)
    Q_PROPERTY(QColor onSurface READ onSurface NOTIFY paletteChanged)
    Q_PROPERTY(QColor muted READ muted NOTIFY paletteChanged)
    Q_PROPERTY(QColor outline READ outline NOTIFY paletteChanged)
    Q_PROPERTY(QColor success READ success NOTIFY paletteChanged)
    Q_PROPERTY(QColor error READ error NOTIFY paletteChanged)
    Q_PROPERTY(QColor warning READ warning NOTIFY paletteChanged)
    // disabledFg and mutedStrong are separate on purpose: a dead control is
    // faded, an unavailable capability keeps full opacity.
    Q_PROPERTY(QColor glyph READ glyph NOTIFY paletteChanged)
    Q_PROPERTY(QColor disabledFg READ disabledFg NOTIFY paletteChanged)
    Q_PROPERTY(QColor mutedStrong READ mutedStrong NOTIFY paletteChanged)
    Q_PROPERTY(QColor primaryHover READ primaryHover NOTIFY paletteChanged)
    Q_PROPERTY(QColor primaryPress READ primaryPress NOTIFY paletteChanged)
    Q_PROPERTY(QColor primaryFill READ primaryFill NOTIFY paletteChanged)
    Q_PROPERTY(QColor warningFill READ warningFill NOTIFY paletteChanged)
    Q_PROPERTY(QColor accentWash24 READ accentWash24 NOTIFY paletteChanged)
    Q_PROPERTY(QColor successFill READ successFill NOTIFY paletteChanged)
    Q_PROPERTY(QColor errorFill READ errorFill NOTIFY paletteChanged)
    Q_PROPERTY(QColor scrim READ scrim NOTIFY paletteChanged)
    Q_PROPERTY(QColor focusRing READ focusRing NOTIFY paletteChanged)
    Q_PROPERTY(QColor outlineSubtle READ outlineSubtle NOTIFY paletteChanged)
    Q_PROPERTY(QColor pulse READ pulse NOTIFY paletteChanged)
    Q_PROPERTY(QColor pulseFill READ pulseFill NOTIFY paletteChanged)
    Q_PROPERTY(QColor pulseEdge READ pulseEdge NOTIFY paletteChanged)

  public:
    explicit ThemeBridge(QObject* parent = nullptr);

    QColor background() const;
    QColor surface() const;
    QColor surfaceDim() const;
    QColor primary() const;
    QColor primaryDark() const;
    QColor onPrimary() const;
    QColor onSurface() const;
    QColor muted() const;
    QColor outline() const;
    QColor success() const;
    QColor error() const;
    QColor warning() const;
    QColor glyph() const;
    QColor disabledFg() const;
    QColor mutedStrong() const;
    QColor primaryHover() const;
    QColor primaryPress() const;
    QColor primaryFill() const;
    QColor warningFill() const;
    QColor accentWash24() const;
    QColor successFill() const;
    QColor errorFill() const;
    QColor scrim() const;
    QColor focusRing() const;
    QColor outlineSubtle() const;
    QColor pulse() const;
    QColor pulseFill() const;
    QColor pulseEdge() const;

    // Call after setActiveAppearance has swapped the palette.
    Q_INVOKABLE void refresh();

    Q_INVOKABLE QColor alpha(const QColor& c, qreal a) const;

  signals:
    void paletteChanged();
};

} // namespace dish::chrome
