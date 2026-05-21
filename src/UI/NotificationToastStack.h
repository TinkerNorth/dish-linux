// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/DishNotification.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <cstdint>

class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QTimer;
class QVBoxLayout;

namespace dish::ui {

class NotificationQueue;

// Bottom-center stacked-toast surface — the visible end of `NotificationQueue`.
// Anchors to the bottom-center of its parent (typically the QMainWindow's
// central widget), shows up to `kMaxVisible` toasts at a time, fades each one
// in on arrival and out either on the auto-dismiss timer, on click, or when
// the queue explicitly dismisses by id.
//
// Older toasts that overflow the visible cap are queued internally and shown
// once a slot frees. The queue order is FIFO — never reordered by severity —
// because the user is reading them top-down and any reshuffle would be
// confusing.
class NotificationToastStack : public QWidget {
    Q_OBJECT
  public:
    static constexpr int kMaxVisible = 3;

    explicit NotificationToastStack(QWidget* parent = nullptr);
    ~NotificationToastStack() override;

    // Wire to a queue. The stack listens for `notificationAdded` /
    // `notificationDismissed` and never holds a strong ref to the queue —
    // both ends are owned by the AppModel / MainWindow.
    void bindTo(NotificationQueue* queue);

  protected:
    // Reposition above the parent's bottom edge on resize.
    bool eventFilter(QObject* watched, QEvent* event) override;

  private slots:
    void onNotificationAdded(const models::DishNotification& notification);
    void onNotificationDismissed(std::uint64_t id);

  private:
    // Wraps one visible toast + its lifecycle bookkeeping (auto-dismiss
    // timer + fade animation). Owns the QLabel; the stack owns the entry.
    struct ToastEntry {
        std::uint64_t id = 0;
        QPointer<QWidget> widget;
        QPointer<QGraphicsOpacityEffect> opacity;
        QPointer<QPropertyAnimation> fadeIn;
        QPointer<QPropertyAnimation> fadeOut;
        QPointer<QTimer> autoDismiss;
    };

    void showNotification(const models::DishNotification& notification);
    void fadeOutAndRemove(std::uint64_t id);
    void reanchor();

    QVBoxLayout* layout_ = nullptr;
    QHash<std::uint64_t, ToastEntry> entries_;
};

} // namespace dish::ui
