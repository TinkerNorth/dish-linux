// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "AppModel.h"
#include "UI/MainWindow.h"
#include "UI/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QTranslator>

#include <sodium.h>

#include <cstdio>

int main(int argc, char* argv[]) {
    if (sodium_init() < 0) {
        (void)std::fprintf(stderr, "dish: libsodium initialisation failed\n");
        return 1;
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TinkerNorth"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tinkernorth.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Dish"));
    QGuiApplication::setDesktopFileName(QStringLiteral("dish"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/dish.svg")));

    // Install a QTranslator keyed off the system locale. The .qm catalogues
    // are baked into the Qt resource tree at :/i18n/ by qt_add_translations
    // (see CMakeLists.txt). If no .qm matches the system locale, Qt falls
    // back to the English source strings inside tr() — exactly the desired
    // behaviour for an English-source app.
    auto* translator = new QTranslator(&app);
    const QString locale = QLocale::system().name(); // e.g. "de_DE", "pt_BR"
    if (translator->load(QStringLiteral("dish_") + locale, QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(translator);
    } else {
        // Try a bare language tag (e.g. "de" matches dish_de.qm).
        const QString lang = locale.section('_', 0, 0);
        if (!lang.isEmpty() &&
            translator->load(QStringLiteral("dish_") + lang, QStringLiteral(":/i18n"))) {
            QCoreApplication::installTranslator(translator);
        }
    }

    dish::ui::applyDishTheme(app);

    dish::AppModel model;
    dish::ui::MainWindow window(&model);
    window.show();
    model.start();

    return app.exec();
}
