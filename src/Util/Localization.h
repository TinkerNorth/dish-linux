// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QString>
#include <QStringLiteral>

class QLocale;
class QTranslator;

namespace dish::i18n {

// Where qt_add_translations() drops the compiled `dish_<locale>.qm` catalogues.
inline QString catalogDirectory() { return QStringLiteral(":/i18n"); }

inline QString catalogPrefix() { return QStringLiteral("dish"); }

// False when the locale has no catalogue and the caller should stay on the
// source strings.
//
// Must stay on QTranslator's locale-aware overload, not a `load("dish_de_DE")`
// filename guess: only the overload walks QLocale::uiLanguages() (what the
// the desktop's language settings populate, so an en_US machine preferring German
// gets German) and does region fallback (de_DE → dish_de, pt_BR → dish_pt_BR).
//
// `directory` is a test seam: the QRC holding the .qm belongs to the Dish
// executable, not to dish_core.
bool loadCatalog(QTranslator& translator, const QLocale& locale,
                 const QString& directory = catalogDirectory());

} // namespace dish::i18n
