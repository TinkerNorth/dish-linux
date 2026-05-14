// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Input/GamepadInputProcessor.h"
#include "Input/SDLGamepadBridge.h"
#include "Models/Models.h"
#include "Network/ConnectionHub.h"
#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"
#include "Util/DisplaySleepInhibitor.h"
#include "Util/ScreenWakeController.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <mutex>

namespace dish {

// Single immutable slice of state consumed by the UI. Mirrors the shape of
// dish-android's MainUiState so all three Dish clients (Android Kotlin,
// macOS Swift, Linux C++) expose one canonical state object instead of a
// fan of independent fields + signals.
struct MainUiState {
    // Named `slotList` (not `slots`) because Qt's moc treats `slots` as a
    // reserved keyword via the `Q_SLOTS` macro and won't parse a member
    // with that name.
    QList<models::ControllerSlot> slotList;
    QList<models::ConnectionSummary> connections;
    std::optional<models::DiscoveredServer> pairingTarget;
};

// Top-level application state. Owns the network + input layers and stitches
// them together the same way the Mac AppModel and Android MainViewModel do.
//
//   * exposes a flat slot list (1 virtual + 1 per attached SDL gamepad),
//   * maintains a slotId -> WifiConnection routing table updated from the Qt
//     main thread and consulted from the SDL gamepad thread on every report,
//   * surfaces a transient errorMessage signal for one-shot toasts.
class AppModel : public QObject {
    Q_OBJECT
  public:
    // Production constructor: builds a FreedesktopScreenSaverInhibitor under
    // the hood. The unique_ptr overload below lets tests inject a fake.
    explicit AppModel(QObject* parent = nullptr);
    AppModel(std::unique_ptr<util::DisplaySleepInhibitor> inhibitor, QObject* parent = nullptr);
    ~AppModel() override;

    net::ConnectionStore* store() { return store_.get(); }
    net::WifiConnectionManager* wifi() { return wifi_; }
    net::ConnectionHub* hub() { return hub_; }
    input::GamepadInputProcessor* processor() { return &processor_; }
    input::SDLGamepadBridge* bridge() { return bridge_; }
    util::ScreenWakeController* wake() { return &wake_; }

    // Single read-only accessor — the UI reads everything off this slice
    // and re-renders on stateChanged().
    const MainUiState& state() const { return state_; }

    // The pairingTarget is a one-shot UI trigger: the dialog reads it on
    // stateChanged() and clears it before showing the prompt.
    void clearPairingTarget();

    void start();

  signals:
    // Emitted after any field of state() changes. Replaces the previous
    // slotsChanged / connectionsChanged / pairingTargetChanged trio.
    void stateChanged();

    // Transient one-shot — errors are events, not state, and are surfaced
    // as toasts/dialogs by MainWindow.
    void errorMessage(const QString& msg);

  private:
    void rebuild();
    void onHubChanged();
    void onBridgeDevicesChanged();
    void onWifiEvent(const net::ConnectionEvent& evt);

    std::unique_ptr<net::ConnectionStore> store_;
    net::WifiConnectionManager* wifi_;
    net::ConnectionHub* hub_;
    input::GamepadInputProcessor processor_;
    input::SDLGamepadBridge* bridge_;
    QTimer* autoReconnectTimer_;
    // Owned in unique_ptr so we can swap a FakeDisplaySleepInhibitor in
    // tests. ScreenWakeController holds a raw back-pointer; lifetime is
    // tied to the AppModel.
    std::unique_ptr<util::DisplaySleepInhibitor> inhibitor_;
    util::ScreenWakeController wake_;

    MainUiState state_;

    // slotId -> active sender. Read on the SDL gamepad thread; written on the
    // Qt main thread. Guarded by routingMtx_ for both directions.
    mutable std::mutex routingMtx_;
    QHash<QString, net::ConnectionHub::ReportSender> routing_;
};

} // namespace dish
