// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "AppModel.h"
#include "UI/CrashHandler.h"
#include "Util/Localization.h"
#include "Util/UnixSignalWatcher.h"
#include "qml/QmlEntryPoint.h"

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QTranslator>

#include <sodium.h>

#include <csignal>
#include <cstdio>

int main(int argc, char* argv[]) {
    // FIRST, before any other subsystem can fault, so a crash still leaves a
    // backtrace behind.
    dish::crash::install();

    if (sodium_init() < 0) {
        // Discarded deliberately: this runs before any logger exists, so a
        // failed write to stderr is unactionable. The exit code is the report.
        (void)std::fprintf(stderr, "dish: libsodium initialisation failed\n");
        return 1;
    }

    // QGuiApplication, not QApplication: no QWidget is ever constructed, so the
    // widgets module stays out of the process.
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TinkerNorth"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tinkernorth.com"));
    QCoreApplication::setApplicationName(QStringLiteral("Dish"));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.tinkernorth.Dish"));

    // loadCatalog walks QLocale::uiLanguages(), so the desktop's preferred UI
    // language wins over the regional format setting — two settings that
    // routinely disagree. English is a real catalogue rather than the
    // untranslated fallback, because %n plural forms have to come from
    // somewhere and a source string can only carry one of them. `static` keeps
    // the translator alive for the lifetime of the app.
    static QTranslator translator;
    if (dish::i18n::loadCatalog(translator, QLocale::system())) {
        QCoreApplication::installTranslator(&translator);
    }

    // Wayland takes the window icon from the .desktop file, but X11 and the
    // Alt-Tab switchers on several compositors still read the window's own.
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/dish.svg")));

    // Inter is bundled (SIL OFL, see packaging/fonts/) so the app matches the
    // design on a machine that does not have it installed. The four statics
    // give the weight ladder the tokens use.
    for (const char* face : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-Medium.ttf",
                             ":/fonts/Inter-SemiBold.ttf", ":/fonts/Inter-Bold.ttf"}) {
        QFontDatabase::addApplicationFont(QLatin1String(face));
    }
    QFont uiFont(QStringLiteral("Inter"));
    uiFont.setPixelSize(13); // the token base; pages override per role
    app.setFont(uiFont);

    // Logout and shutdown arrive as SIGTERM, whose default disposition kills
    // the process where it stands: ~AppModel never runs, so the input thread is
    // not stopped and QSettings never flushes what the session changed. Quit
    // the loop instead and let main unwind normally.
    const auto quitSignals = dish::util::installSignalWatcher({SIGINT, SIGTERM, SIGHUP});
    if (quitSignals) {
        QObject::connect(quitSignals.get(), &dish::util::UnixSignalWatcher::signalled, &app,
                         [](int) { QCoreApplication::quit(); });
    }

    // runQmlApp owns the engine and exposes the model to QML as the `App`
    // context property.
    dish::AppModel model;
    model.start();
    return dish::qml::runQmlApp(model);
}
