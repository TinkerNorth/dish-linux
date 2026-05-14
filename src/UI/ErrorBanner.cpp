// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ErrorBanner.h"

#include "Theme.h"

#include <QTimer>

namespace dish::ui {

namespace {
constexpr int kAutoDismissMs = 6'000;
} // namespace

ErrorBanner::ErrorBanner(QWidget* parent) : QLabel(parent) {
    setWordWrap(true);
    setAlignment(Qt::AlignCenter);
    setCursor(Qt::PointingHandCursor);
    setVisible(false);
    setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: %2; "
                                 "padding: 10px 16px; font-weight: 500; border-radius: 6px; }")
                      .arg(hex(Theme::error), hex(Theme::onSurface)));

    dismissTimer_ = new QTimer(this);
    dismissTimer_->setSingleShot(true);
    dismissTimer_->setInterval(kAutoDismissMs);
    QObject::connect(dismissTimer_, &QTimer::timeout, this, [this] { setVisible(false); });
}

void ErrorBanner::showError(const QString& message) {
    setText(message);
    setVisible(true);
    dismissTimer_->start();
}

void ErrorBanner::mousePressEvent(QMouseEvent* /*event*/) {
    setVisible(false);
    dismissTimer_->stop();
}

} // namespace dish::ui
