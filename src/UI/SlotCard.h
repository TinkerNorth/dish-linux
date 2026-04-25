// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;

namespace dish::ui {

// One row in the slots list. Mirrors the Mac SlotCard / Android slot row.
class SlotCard : public QFrame {
    Q_OBJECT
  public:
    explicit SlotCard(QWidget* parent = nullptr);

    void setSlot(const models::ControllerSlot& slot,
                 const QList<models::ConnectionSummary>& available);

  signals:
    void bindRequested(const QString& slotId, const QString& connectionId);
    void unbindRequested(const QString& slotId);

  private:
    void onBindClicked();

    QLabel* nameLabel_;
    QLabel* boundLabel_;
    QLabel* dot_;
    QPushButton* bindButton_;

    models::ControllerSlot slot_;
    QList<models::ConnectionSummary> available_;
};

} // namespace dish::ui
