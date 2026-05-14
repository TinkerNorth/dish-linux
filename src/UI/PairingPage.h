// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace dish::ui {

// Pairing page — collects the 6-digit PIN. Lives inside the MainWindow
// QStackedWidget. Replaces the prior modal PairingDialog.
class PairingPage : public QWidget {
    Q_OBJECT
  public:
    explicit PairingPage(QWidget* parent = nullptr);
    void setServer(const models::DiscoveredServer& server);
    // Drive the pending state from the controller: spinner on, PIN entry and
    // Pair locked. Cancel stays active so the user can always back out.
    void setPending(bool pending);

  signals:
    void pairRequested(const models::DiscoveredServer& server, const QString& pin);
    void cancelRequested();

  private:
    void submit();
    void updatePairEnabled();

    QLabel* title_;
    QLabel* message_;
    QLineEdit* pinEdit_;
    QPushButton* pairBtn_;
    QProgressBar* spinner_;
    models::DiscoveredServer server_;
    bool pending_ = false;
};

} // namespace dish::ui
