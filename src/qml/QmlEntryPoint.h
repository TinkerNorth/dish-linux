// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Registers the QML singletons, loads Main.qml and runs. Kept out of main.cpp
// so the QML headers stay clear of its core init (crash handler, libsodium).

#pragma once

namespace dish {
class AppModel;
}

namespace dish::qml {

// Returns the exec() code. `model` must already be started, and must outlive
// the engine — the AppViewModel wrapping it is a context property.
int runQmlApp(dish::AppModel& model);

} // namespace dish::qml
