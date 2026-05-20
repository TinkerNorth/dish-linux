// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QIcon>
#include <QPixmap>
#include <QString>

class QLabel;

namespace dish::ui {

// Source family for a BrandIcon glyph. Matches `BrandIconKind` in
// dish-mac's BrandIcon.swift and `ConnectionKind` in dish-android.
enum class BrandIconKind {
    Dish,
    Satellite,
    Bluetooth,
};

// Maps a connection's LinkState onto the right `:/brand/...svg` resource
// from packaging/dish.qrc (or resources/icons.qrc on Linux) and renders it
// into a pixmap at the requested logical px size. The kept aspect ratio
// pulls from the source 64u canvas; oversample by 2× internally so HiDPI
// screens stay crisp.
//
// Same policy lives in three other places — keep these in sync:
//   • dish-android: ConnectionsActivity.rowGlyphRes()
//   • dish-mac:     BrandIcon.dish(for:) / direct .connected / .off picks
//   • satellite/web/dashboard.js: DEVICE_LINK_STATE_ICON
QString brandIconResource(BrandIconKind kind, models::LinkState state);

// Render a brand SVG resource (e.g. ":/brand/dish-connected.svg") into a
// QPixmap. Width = height = `px` × the device pixel ratio of `target` so the
// glyph stays sharp on HiDPI. Falls back to a transparent pixmap if Qt SVG
// support is unavailable at runtime.
QPixmap brandPixmap(const QString& resource, int px, const QWidget* target);

// Convenience: paint a brand glyph straight into a QLabel and size it.
void setBrandIcon(QLabel* label, BrandIconKind kind, models::LinkState state, int px);

// Build a QIcon for a brand glyph at `px`. Used for QAction / QMenu items
// (e.g. SlotCard::onBindClicked's bind picker) where we want the same v6
// brand iconography as the rest of the UI rather than a system-default
// QStyle::StandardPixmap.
QIcon brandIcon(BrandIconKind kind, models::LinkState state, int px,
                const QWidget* target);

} // namespace dish::ui
