// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The suite provides its own main so ONE QCoreApplication spans the whole
// session and is destroyed before Qt's own exit handlers run.
//
// A function-local `static QCoreApplication` looks like it works — the tests
// pass — and then segfaults during `__run_exit_handlers`, because a static
// destroyed at exit is torn down alongside the library statics that still
// reference it. Anything needing an event loop or an app instance (the HTTP
// client's app-static factory, QSettings' organisation-scoped paths) gets one
// for the whole run instead.
//
// QCoreApplication, not QGuiApplication: nothing under test constructs a
// widget, a window or a font database, so the suite needs no display.

#include <catch2/catch_session.hpp>

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TinkerNorth"));
    QCoreApplication::setApplicationName(QStringLiteral("DishTests"));
    return Catch::Session().run(argc, argv);
}
