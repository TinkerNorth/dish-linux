// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QLabel>

class QTimer;

namespace dish::ui {

// Inline error banner styled with Theme::error. Mirrors the role of the
// dish-mac alert(...) and dish-android Toast — a transient, non-modal
// surface for AppModel::errorMessage. Click anywhere on the banner to
// dismiss; otherwise auto-hides after six seconds.
class ErrorBanner : public QLabel {
    Q_OBJECT
  public:
    explicit ErrorBanner(QWidget* parent = nullptr);

    void showError(const QString& message);

  protected:
    void mousePressEvent(QMouseEvent* event) override;

  private:
    QTimer* dismissTimer_;
};

} // namespace dish::ui
