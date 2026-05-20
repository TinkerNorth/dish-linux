// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace dish::ui {

class DishInFlightButton;

// Pairing page — collects the 6-digit PIN. Lives inside the MainWindow
// QStackedWidget. Replaces the prior modal PairingDialog.
class PairingPage : public QWidget {
    Q_OBJECT
  public:
    explicit PairingPage(QWidget* parent = nullptr);
    void setServer(const models::DiscoveredServer& server);
    // Drive the pending state from the controller: spinner on, PIN entry and
    // Pair locked. Cancel stays disabled while pairing is in flight — matches
    // the design spec (`PairingSheet.swift`): cancel is gated during submit
    // so the user can't tear the request out mid-handshake.
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
    QPushButton* cancelBtn_;
    // DishInFlightButton renders DishSpinner + "Pairing…" while the
    // server registration is in flight; it's a plain "Pair" otherwise.
    DishInFlightButton* pairBtn_;
    models::DiscoveredServer server_;
    bool pending_ = false;
};

} // namespace dish::ui
