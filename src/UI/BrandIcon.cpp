// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "BrandIcon.h"

#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QWidget>

namespace dish::ui {

QString brandIconResource(BrandIconKind kind, models::LinkState state) {
    using S = models::LinkState;
    switch (kind) {
    case BrandIconKind::Dish:
        switch (state) {
        case S::Connected: return QStringLiteral(":/brand/dish-connected.svg");
        case S::Connecting: return QStringLiteral(":/brand/dish-scanning-animated.svg");
        case S::Saved:
        case S::Stale: return QStringLiteral(":/brand/dish-off.svg");
        default: return QStringLiteral(":/brand/dish.svg");
        }
    case BrandIconKind::Satellite:
        switch (state) {
        case S::Connected: return QStringLiteral(":/brand/satellite-connected.svg");
        case S::Connecting: return QStringLiteral(":/brand/satellite-broadcasting-animated.svg");
        case S::Saved:
        case S::Stale: return QStringLiteral(":/brand/satellite-off.svg");
        default: return QStringLiteral(":/brand/satellite.svg");
        }
    case BrandIconKind::Bluetooth:
        switch (state) {
        case S::Connected: return QStringLiteral(":/brand/bluetooth-connected.svg");
        case S::Connecting: return QStringLiteral(":/brand/bluetooth-searching.svg");
        case S::Saved:
        case S::Stale: return QStringLiteral(":/brand/bluetooth-off.svg");
        default: return QStringLiteral(":/brand/bluetooth.svg");
        }
    }
    return QStringLiteral(":/brand/dish.svg");
}

QPixmap brandPixmap(const QString& resource, int px, const QWidget* target) {
    const qreal dpr = target != nullptr ? target->devicePixelRatioF() : 1.0;
    const int physical = static_cast<int>(px * dpr);
    QSvgRenderer renderer(resource);
    if (!renderer.isValid()) { return QPixmap(); }
    QPixmap pm(physical, physical);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(0, 0, physical, physical));
    pm.setDevicePixelRatio(dpr);
    return pm;
}

void setBrandIcon(QLabel* label, BrandIconKind kind, models::LinkState state, int px) {
    if (label == nullptr) { return; }
    label->setPixmap(brandPixmap(brandIconResource(kind, state), px, label));
    label->setFixedSize(px, px);
}

QIcon brandIcon(BrandIconKind kind, models::LinkState state, int px,
                const QWidget* target) {
    return QIcon(brandPixmap(brandIconResource(kind, state), px, target));
}

} // namespace dish::ui
