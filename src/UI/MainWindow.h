// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QMainWindow>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace dish {
class AppModel;
}

namespace dish::ui {

class ConnectionsPage;
class ErrorBanner;
class NotificationQueue;
class NotificationToastStack;
class PairingPage;
class SettingsView;

// Dashboard window. Owns a QStackedWidget that swaps between the Dashboard,
// Connections, and Pairing pages — replaces the prior stack of modal dialogs.
// Mirrors dish-mac MainView and dish-android activity_main.
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(AppModel* model, QWidget* parent = nullptr);

  private:
    void buildDashboardPage();
    void onStateChanged();
    void rebuildHeader();
    void rebuildSlotList();
    void maybeShowPairingPage();
    void onError(const QString& msg);
    void onManageClicked();
    void onSettingsClicked();
    void onBindRequested(const QString& slotId, const QString& connectionId);
    void onUnbindRequested(const QString& slotId);
    void showDashboard();
    void returnFromPairing();
    void onPairSubmit(const models::DiscoveredServer& server, const QString& pin);

    AppModel* model_;

    QStackedWidget* stack_;
    QWidget* dashboardPage_;
    ConnectionsPage* connectionsPage_;
    PairingPage* pairingPage_;
    SettingsView* settingsPage_;
    // Inline error strip — kept around for one-off in-page surfaces that
    // shouldn't share screen real estate with the bottom-anchored toast
    // stack. Today it is no longer driven by AppModel::errorMessage (that
    // routes through `notificationQueue_` instead); a future page-local
    // error surface can still construct one.
    ErrorBanner* errorBanner_;
    // The new feedback channel: every `AppModel::errorMessage` lands here,
    // which the toast stack renders as a brand-styled stacked banner.
    NotificationQueue* notificationQueue_;
    NotificationToastStack* toastStack_;
    QProgressBar* dashboardSpinner_;
    QWidget* pairingReturnPage_ = nullptr;
    bool awaitingPair_ = false;
    QString awaitingPairConnectionId_;

    QLabel* statusDot_;
    QLabel* statusText_;
    QLabel* summaryText_;
    QPushButton* settingsButton_;
    QPushButton* manageButton_;
    QVBoxLayout* slotsLayout_;
    QLabel* slotsEmpty_;
};

} // namespace dish::ui
