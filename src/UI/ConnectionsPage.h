// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace dish {
class AppModel;
}

namespace dish::ui {

class DishInFlightButton;

// Connections page — discovery, connect, forget. Lives inside the
// MainWindow QStackedWidget. Replaces the prior modal ConnectionsDialog.
class ConnectionsPage : public QWidget {
    Q_OBJECT
  public:
    ConnectionsPage(AppModel* model, QWidget* parent = nullptr);

  signals:
    void backRequested();

  private:
    void rebuildLists();
    // Push the latest in-flight state into the Scan + Connect buttons —
    // toggles spinner visibility, label text and enabled-ness from the
    // current AppModel/WifiConnectionManager observation.
    void refreshButtonStates();
    // Recompute only the remembered-row texts (latency / state tags) without
    // clearing the lists, so selection survives the 1 Hz telemetry tick.
    void refreshTelemetryTags();
    QString rememberedRowText(const dish::models::RememberedWifi& r) const;
    void onScanClicked();
    void onConnectClicked();
    void onForgetClicked();

    // True if the currently-selected discovered server matches a known
    // connection in LinkState::Connecting. Drives the in-Connect-button
    // spinner.
    bool connectingForSelection() const;

    AppModel* model_;
    QListWidget* discoveredList_;
    QListWidget* rememberedList_;
    // Scan + Connect use DishInFlightButton so the spinner lives *inside*
    // the button per the design spec; forget stays a vanilla QPushButton.
    DishInFlightButton* scanButton_;
    DishInFlightButton* connectButton_;
    QPushButton* forgetButton_;
    QLabel* statusLabel_;
};

} // namespace dish::ui
