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
            "QPushButton:disabled { color: %6; border-color: %6; }"
            "QPushButton#primary { background-color: %5; color: %7; border: none; }"
            "QPushButton#primary:hover { background-color: %8; }"
            "QListWidget, QTreeWidget { background-color: %3; border: 1px solid %4; "
            "                          border-radius: 8px; padding: 4px; }"
            "QStatusBar { background-color: %3; color: %6; }"
            "QLineEdit { background-color: %3; color: %2; border: 1px solid %4; "
            "           border-radius: 6px; padding: 6px 8px; }"
            "QLineEdit:focus { border-color: %5; }")
            .arg(hex(Theme::background), hex(Theme::onSurface), hex(Theme::surface),
                 hex(Theme::outline), hex(Theme::primary), hex(Theme::muted), hex(Theme::onPrimary),
                 hex(Theme::primaryDark));
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

} // namespace dish::ui
