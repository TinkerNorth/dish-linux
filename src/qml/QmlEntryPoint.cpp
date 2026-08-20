// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/QmlEntryPoint.h"

#include "AppModel.h"
#include "qml/AppViewModel.h"
#include "qml/ConnectionListModel.h"
#include "qml/SlotListModel.h"
#include "qml/chrome/ThemeBridge.h"
#include "qml/chrome/TokensBridge.h"
#include "UI/common/ExternalLink.h"

#include "UI/Theme.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QWindow>
#include <qqml.h>

namespace dish::qml {

int runQmlApp(dish::AppModel& model) {
    // Every kit control is custom-painted from Theme tokens, so the platform
    // styles would only fight them.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Closing the window is allowed to mean "keep streaming", so the process
    // must outlive it. Main.qml's approveClose() is the one real quit path.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    // Uncreatable: instances are only ever vended through App.slotModel /
    // App.connectionModel, but QML must be able to name the type in a delegate.
    qmlRegisterUncreatableType<dish::qml::SlotListModel>(
        "Dish.Chrome", 1, 0, "SlotListModel",
        QStringLiteral("SlotListModel is owned by AppViewModel"));
    qmlRegisterUncreatableType<dish::qml::ConnectionListModel>(
        "Dish.Chrome", 1, 0, "ConnectionListModel",
        QStringLiteral("ConnectionListModel is owned by AppViewModel"));

    dish::qml::AppViewModel appVm(&model);

    // model.start() already ran the ThemeController; re-resolving here
    // unconditionally (idempotent) makes the active palette provably match the
    // stored mode before the Theme singleton is first read.
    {
        const auto mode = model.themeStore()->mode();
        const auto appearance = mode == dish::source::ThemeMode::Light ? dish::ui::Appearance::Light
                                : mode == dish::source::ThemeMode::Dark
                                    ? dish::ui::Appearance::Dark
                                    : dish::ui::detectSystemAppearance();
        dish::ui::setActiveAppearance(appearance);
    }

    // Registered BY INSTANCE rather than via QML_SINGLETON so the names reach
    // the engine even when link-time optimization strips the generated
    // QQmlModuleRegistration initializer. Declared before the engine so they
    // outlive it; CppOwnership so QML never deletes them.
    auto* themeBridge = new dish::chrome::ThemeBridge(qApp);
    auto* tokensBridge = new dish::chrome::TokensBridge(qApp);
    QQmlEngine::setObjectOwnership(themeBridge, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(tokensBridge, QQmlEngine::CppOwnership);
    qmlRegisterSingletonInstance("Dish.Chrome", 1, 0, "Theme", themeBridge);
    qmlRegisterSingletonInstance("Dish.Chrome", 1, 0, "Tokens", tokensBridge);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &appVm);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, qApp,
        [themeBridge](QObject* obj, const QUrl&) {
            if (qobject_cast<QQuickWindow*>(obj) == nullptr) { return; }
            // Any binding that evaluated before the palette settled re-reads
            // now that the window exists.
            if (themeBridge) { themeBridge->refresh(); }
        },
        Qt::DirectConnection);

    // A false return falls through to App.errorMessage (the QML toast channel).
    appVm.setExternalOpenSink([](const QString& url) { return dish::ui::openExternalUrl(url); });

    appVm.setThemeAppliedSink([themeBridge](bool) {
        if (themeBridge) { themeBridge->refresh(); }
    });

    engine.loadFromModule(QStringLiteral("Dish.Chrome"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) { return 1; }

    return QGuiApplication::exec();
}

} // namespace dish::qml
