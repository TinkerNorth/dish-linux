// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one shared "open a link" edge. Failures are reported to the caller; QML
// surfaces them through App.errorMessage.

#pragma once

#include <QString>

namespace dish::ui {

// True iff the open was handed off successfully to the OS.
bool openExternalUrl(const QString& url);

} // namespace dish::ui
