// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// QML type identities for the view-model types that live in dish_core.
//
// dish_core deliberately links no Qml: the tests exercise AppViewModel and the
// two role models without the Quick stack, so those classes cannot carry
// QML_ELEMENT themselves. These QML_FOREIGN declarations do it for them from
// the Dish target, which is the Dish.Chrome module's backing target, so
// qmltyperegistrar writes the types into the module's generated qmltypes. That
// is what lets qmllint resolve `App.statusText` and friends statically instead
// of reporting every one of them as an unqualified access.
//
// They do NOT replace the runtime registrations in QmlEntryPoint. Under this
// target's LTO the generated QQmlModuleRegistration initializer is stripped,
// so a declarative-only name may never reach the engine at all. App's create()
// therefore hands back the very instance QmlEntryPoint registered by value:
// whichever of the two registrations the engine resolves, QML sees one object.

#pragma once

#include "qml/AppViewModel.h"
#include "qml/ConnectionListModel.h"
#include "qml/SlotListModel.h"

#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace dish::chrome {

// The application view model, spelled `App` in QML.
struct AppViewModelForeign {
    Q_GADGET
    QML_FOREIGN(dish::qml::AppViewModel)
    QML_NAMED_ELEMENT(App)
    QML_SINGLETON

  public:
    // QmlEntryPoint owns the instance on its stack and publishes it here before
    // the engine exists, so the factory below can never be asked first.
    static void setInstance(dish::qml::AppViewModel* instance) { s_instance = instance; }

    static dish::qml::AppViewModel* create(QQmlEngine*, QJSEngine*) { return s_instance; }

  private:
    inline static dish::qml::AppViewModel* s_instance = nullptr;
};

// Uncreatable: instances are only ever vended through App.slotModel /
// App.connectionModel, but QML must be able to name the type in a delegate.
struct SlotListModelForeign {
    Q_GADGET
    QML_FOREIGN(dish::qml::SlotListModel)
    QML_NAMED_ELEMENT(SlotListModel)
    QML_UNCREATABLE("SlotListModel is owned by AppViewModel")
};

struct ConnectionListModelForeign {
    Q_GADGET
    QML_FOREIGN(dish::qml::ConnectionListModel)
    QML_NAMED_ELEMENT(ConnectionListModel)
    QML_UNCREATABLE("ConnectionListModel is owned by AppViewModel")
};

} // namespace dish::chrome
