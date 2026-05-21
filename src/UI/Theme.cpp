// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Theme.h"

#include <QPalette>

namespace dish::ui {

QString hex(QRgb c) {
    return QStringLiteral("#%1%2%3")
        .arg(qRed(c), 2, 16, QLatin1Char('0'))
        .arg(qGreen(c), 2, 16, QLatin1Char('0'))
        .arg(qBlue(c), 2, 16, QLatin1Char('0'));
}

void applyDishTheme(QApplication& app) {
    QPalette p;
    const QColor bg(Theme::background);
    const QColor surface(Theme::surface);
    const QColor onSurface(Theme::onSurface);
    const QColor primary(Theme::primary);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, onSurface);
    p.setColor(QPalette::Base, surface);
    p.setColor(QPalette::AlternateBase, QColor(Theme::surfaceDim));
    p.setColor(QPalette::ToolTipBase, surface);
    p.setColor(QPalette::ToolTipText, onSurface);
    p.setColor(QPalette::Text, onSurface);
    p.setColor(QPalette::Button, surface);
    p.setColor(QPalette::ButtonText, onSurface);
    p.setColor(QPalette::Highlight, primary);
    p.setColor(QPalette::HighlightedText, QColor(Theme::onPrimary));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(Theme::muted));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(Theme::muted));
    app.setPalette(p);

    // Disabled-state alpha — design spec (ds-components.jsx → Button) calls
    // for `opacity: 0.4` on disabled buttons. Qt stylesheets can't apply a
    // CSS-style `opacity` to a widget, but rgba() colors at 40 % alpha on the
    // primary tint reproduce the same visual on this dark background. Press
    // feedback (hover/pressed selectors) is intentionally omitted from the
    // `:disabled` cascade so a stray click on a not-tappable button doesn't
    // flash the primary tint. Hex strings:
    //   * `rgba(79,227,255,0.4)`  — primary @ 40 % (outlined disabled fg + border)
    //   * `rgba(6,8,24,0.4)`      — onPrimary @ 40 % (filled #primary disabled fg)
    const QString qss =
        QStringLiteral(
            "QMainWindow, QDialog { background-color: %1; }"
            "QWidget { color: %2; font-family: 'Inter','Roboto',sans-serif; font-size: 13px; }"
            "QFrame#card { background-color: %3; border: 1px solid %4; border-radius: 8px; }"
            "QLabel#section { font-family: monospace; color: %5; letter-spacing: 1.5px; "
            "                font-size: 11px; }"
            "QPushButton { background: transparent; color: %5; border: 1px solid %5; "
            "             border-radius: 6px; padding: 6px 12px; font-weight: 500; }"
            "QPushButton:hover { background-color: rgba(79,227,255,0.12); }"
            "QPushButton:pressed { background-color: rgba(79,227,255,0.18); }"
            "QPushButton:disabled { color: rgba(79,227,255,0.4); "
            "                      border-color: rgba(79,227,255,0.4); "
            "                      background: transparent; }"
            "QPushButton#primary { background-color: %5; color: %7; border: none; }"
            "QPushButton#primary:hover { background-color: %8; }"
            "QPushButton#primary:disabled { background-color: rgba(79,227,255,0.4); "
            "                              color: rgba(6,8,24,0.4); border: none; }"
            "QListWidget, QTreeWidget { background-color: %3; border: 1px solid %4; "
            "                          border-radius: 8px; padding: 4px; }"
            "QStatusBar { background-color: %3; color: %6; }"
            "QLineEdit { background-color: %3; color: %2; border: 1px solid %4; "
            "           border-radius: 6px; padding: 6px 8px; }"
            "QLineEdit:focus { border-color: %5; }"
            "QProgressBar { background-color: %3; border: 1px solid %4; border-radius: 2px; }"
            "QProgressBar::chunk { background-color: %5; border-radius: 2px; }")
            .arg(hex(Theme::background), hex(Theme::onSurface), hex(Theme::surface),
                 hex(Theme::outline), hex(Theme::primary), hex(Theme::muted), hex(Theme::onPrimary),
                 hex(Theme::primaryDark), hex(Theme::surfaceDim));
    app.setStyleSheet(qss);
}

QString sectionHeaderQss() {
    return QStringLiteral(
               "font-family: monospace; color: %1; letter-spacing: 1.5px; font-size: 11px;")
        .arg(hex(Theme::primary));
}

QString outlinedButtonQss() {
    return QStringLiteral(
               "background: transparent; color: %1; border: 1px solid %1; border-radius: 6px; "
               "padding: 6px 12px;")
        .arg(hex(Theme::primary));
}

QString dotQss(QRgb color) {
    return QStringLiteral("background-color: %1; border-radius: 4px;").arg(hex(color));
}

QString capabilityChipQss(bool on) {
    // `on`  : primary text on a faint primary fill, no border — "feature live".
    // `off` : muted text, transparent fill, muted border — "hardware absent".
    // The faint fill reuses the same rgba(79,227,255,0.14) tint the QPushButton
    // hover state uses, so the chip sits in the established palette.
    if (on) {
        return QStringLiteral("color: %1; background-color: rgba(79,227,255,0.14); "
                              "border: 1px solid transparent; border-radius: 5px; "
                              "padding: 2px 7px; font-size: 10px; font-weight: 500;")
            .arg(hex(Theme::primary));
    }
    return QStringLiteral("color: %1; background-color: transparent; "
                          "border: 1px solid %1; border-radius: 5px; "
                          "padding: 2px 7px; font-size: 10px; font-weight: 500;")
        .arg(hex(Theme::muted));
}

QString batteryChipQss(bool lowBattery) {
    // Same pill geometry as capabilityChipQss's `on` branch. A healthy battery
    // reuses the cyan `primary` tint; a low battery (< ~15 %) swaps to the
    // amber `warning` token so the player can't miss it. The faint fill alpha
    // is kept as a literal rgba() — QSS needs the alpha inline and there is no
    // half-alpha colour token.
    if (lowBattery) {
        return QStringLiteral("color: %1; background-color: rgba(245,158,11,0.16); "
                              "border: 1px solid transparent; border-radius: 5px; "
                              "padding: 2px 7px; font-size: 10px; font-weight: 600;")
            .arg(hex(Theme::warning));
    }
    return QStringLiteral("color: %1; background-color: rgba(79,227,255,0.14); "
                          "border: 1px solid transparent; border-radius: 5px; "
                          "padding: 2px 7px; font-size: 10px; font-weight: 500;")
        .arg(hex(Theme::primary));
}

} // namespace dish::ui
