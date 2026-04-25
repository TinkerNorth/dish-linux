// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QDialog>

class QListWidget;
class QPushButton;
class QLabel;

namespace dish {
class AppModel;
}

namespace dish::ui {

// "Manage connections" sheet — discovery, connect, forget. Mirrors the Mac
// ConnectionsView.
class ConnectionsDialog : public QDialog {
    Q_OBJECT
  public:
    ConnectionsDialog(AppModel* model, QWidget* parent = nullptr);

  private:
    void rebuildLists();
    void onScanClicked();
    void onConnectClicked();
    void onForgetClicked();

    AppModel* model_;
    QListWidget* discoveredList_;
    QListWidget* rememberedList_;
    QPushButton* scanButton_;
    QLabel* statusLabel_;
};

} // namespace dish::ui
