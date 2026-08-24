// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "source/tray/StatusNotifierTrayIcon.h"

#include <QImage>
#include <QString>

namespace dish::source {

inline constexpr auto kSniIconName = "com.tinkernorth.Dish";

QString sniIconName(bool themeHasIcon);
SniIconPixmapList sniTrayPixmaps();
SniIconPixmap toSniPixmap(const QImage& source);

} // namespace dish::source
