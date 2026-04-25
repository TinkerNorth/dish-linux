// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "AppModel.h"
#include "UI/MainWindow.h"
#include "UI/Theme.h"

#include <QApplication>
#include <QGuiApplication>

#include <sodium.h>

#include <cstdio>

int main(int argc, char* argv[]) {
    if (sodium_init() < 0) {
        std::fprintf(stderr, "dish: libsodium initialisation failed\n");
        return 1;
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TinkerNorth"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tinkernorth.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Dish"));
    QGuiApplication::setDesktopFileName(QStringLiteral("dish"));

    dish::ui::applyDishTheme(app);

    dish::AppModel model;
    dish::ui::MainWindow window(&model);
    window.show();
    model.start();

    return app.exec();
}
