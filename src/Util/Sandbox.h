// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Where the process runs. A Flatpak is read two ways because either alone can
// be absent: FLATPAK_ID is set by the launcher, /.flatpak-info by the sandbox.

#pragma once

#include <QFile>
#include <QString>
#include <QtGlobal>

namespace dish::util {

inline bool runningInFlatpak() {
    return qEnvironmentVariableIsSet("FLATPAK_ID") ||
           QFile::exists(QStringLiteral("/.flatpak-info"));
}

} // namespace dish::util
