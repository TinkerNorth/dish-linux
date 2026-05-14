// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace dish {
class AppModel;
}

namespace dish::ui {

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
    void onScanClicked();
    void onConnectClicked();
    void onForgetClicked();

    AppModel* model_;
    QListWidget* discoveredList_;
    QListWidget* rememberedList_;
    QPushButton* scanButton_;
    QPushButton* connectButton_;
    QPushButton* forgetButton_;
    QProgressBar* scanSpinner_;
    QLabel* statusLabel_;
};

} // namespace dish::ui
