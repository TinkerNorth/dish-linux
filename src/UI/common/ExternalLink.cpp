// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "UI/common/ExternalLink.h"

#include <QDesktopServices>
#include <QUrl>

namespace dish::ui {

bool openExternalUrl(const QString& url) { return QDesktopServices::openUrl(QUrl(url)); }

} // namespace dish::ui
