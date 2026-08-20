// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/Localization.h"

#include <QLocale>
#include <QTranslator>

namespace dish::i18n {

bool loadCatalog(QTranslator& translator, const QLocale& locale, const QString& directory) {
    return translator.load(locale, catalogPrefix(), QStringLiteral("_"), directory,
                           QStringLiteral(".qm"));
}

} // namespace dish::i18n
