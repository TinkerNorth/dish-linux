// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QList>
#include <QMainWindow>

class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace dish {
class AppModel;
}

namespace dish::ui {

// Dashboard window — mirrors dish-mac MainView and dish-android activity_main.
// Status header, controllers section (one SlotCard per slot), telemetry
// footer, and a "Manage" button that opens ConnectionsDialog.
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(AppModel* model, QWidget* parent = nullptr);

  private:
    void onStateChanged();
    void rebuildHeader();
    void rebuildSlotList();
    void showPairingPrompt();
    void onError(const QString& msg);
    void onTelemetryTick();
    void onManageClicked();
    void onBindRequested(const QString& slotId, const QString& connectionId);
    void onUnbindRequested(const QString& slotId);

    AppModel* model_;

    QLabel* statusDot_;
    QLabel* statusText_;
    QLabel* summaryText_;
    QPushButton* manageButton_;
    QVBoxLayout* slotsLayout_;
    QLabel* slotsEmpty_;
    QLabel* telemetryLeft_;
    QLabel* telemetryRight_;

    QTimer* telemetryTimer_;
    quint64 telemetryTotal_ = 0;
};

} // namespace dish::ui
