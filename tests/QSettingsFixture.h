// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One shared store that the cert-pin, shared-key and remembered-list repos
// co-tenant by key prefix, which is what makes the namespace-isolation and
// selective-clear assertions meaningful.

#pragma once

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <QUuid>

#include <memory>

namespace dish::test {

// A unique temp INI, never the real HKCU registry, so tests cannot leak into
// each other or the developer's machine.
inline std::shared_ptr<QSettings> makeSharedSettings() {
    const QString path = QDir::tempPath() + QStringLiteral("/dish-test-") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         QStringLiteral(".ini");
    return std::shared_ptr<QSettings>(new QSettings(path, QSettings::IniFormat),
                                      [path](QSettings* s) {
                                          s->sync();
                                          delete s;
                                          QFile::remove(path);
                                      });
}

} // namespace dish::test
