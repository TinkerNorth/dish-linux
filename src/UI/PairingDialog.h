// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QDialog>

class QLineEdit;

namespace dish::ui {

// Modal sheet shown when the satellite server requires a fresh pairing PIN.
// Mirrors dish-mac/UI/PairingSheet.swift.
class PairingDialog : public QDialog {
    Q_OBJECT
  public:
    PairingDialog(const models::DiscoveredServer& server, QWidget* parent = nullptr);
    QString pin() const;

  private:
    QLineEdit* pinEdit_;
};

} // namespace dish::ui
