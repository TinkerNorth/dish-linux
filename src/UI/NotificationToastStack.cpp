// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "NotificationToastStack.h"

#include "NotificationQueue.h"
#include "Theme.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QShowEvent>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>

namespace dish::ui {

namespace {

constexpr int kFadeInMs = 180;
constexpr int kFadeOutMs = 220;
constexpr int kStackHorizontalPadding = 20;
constexpr int kStackBottomPadding = 24;
constexpr int kStackMaxWidth = 480;

QString backgroundColorFor(models::NotificationSeverity severity) {
    switch (severity) {
    case models::NotificationSeverity::Info:
        return hex(Theme::primary);
    case models::NotificationSeverity::Success:
        return hex(Theme::success);
    case models::NotificationSeverity::Warn:
        return hex(Theme::warning);
    case models::NotificationSeverity::Error:
        return hex(Theme::error);
    }
    return hex(Theme::primary);
}

QString toastQss(models::NotificationSeverity severity) {
    // Same shape the ErrorBanner used: tinted background, on-surface text,
    // pill-rounded. Keeps the visual idiom familiar.
    return QStringLiteral("QLabel { background-color: %1; color: %2; "
                          "padding: 10px 16px; font-weight: 500; border-radius: 6px; }")
        .arg(backgroundColorFor(severity), hex(Theme::onSurface));
}

} // namespace

NotificationToastStack::NotificationToastStack(QWidget* parent) : QWidget(parent) {
    // The stack is a transparent pass-through; only the child QLabel toasts
    // paint anything. Mouse events fall through to the dashboard underneath
    // unless they hit a toast (which carries PointingHandCursor and a
    // mousePressEvent that dismisses it).
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setStyleSheet(QStringLiteral("background: transparent;"));

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);
    // New toasts are appended to the bottom so the most-recent banner sits
    // closest to the user's eye-line at the dashboard bottom.
    layout_->setDirection(QBoxLayout::TopToBottom);

    if (parent != nullptr) { parent->installEventFilter(this); }
    reanchor();
    // Start hidden — appear only when something is enqueued. Keeps the dashboard
    // visually clean while idle.
    raise();
    setVisible(false);
}

NotificationToastStack::~NotificationToastStack() = default;

void NotificationToastStack::bindTo(NotificationQueue* queue) {
    if (queue == nullptr) { return; }
    QObject::connect(queue, &NotificationQueue::notificationAdded, this,
                     &NotificationToastStack::onNotificationAdded);
    QObject::connect(queue, &NotificationQueue::notificationDismissed, this,
                     &NotificationToastStack::onNotificationDismissed);
}

bool NotificationToastStack::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parent() && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        reanchor();
        return false;
    }
    // Click-to-dismiss for individual toast labels. Each dismissible toast
    // carries a `dish_notification_id` dynamic property pointing at the
    // queue entry; on mouse press we route back through fadeOutAndRemove.
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            const auto v = w->property("dish_notification_id");
            if (v.isValid()) {
                fadeOutAndRemove(v.toULongLong());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NotificationToastStack::reanchor() {
    auto* p = parentWidget();
    if (p == nullptr) { return; }
    const int availableWidth = p->width() - 2 * kStackHorizontalPadding;
    const int width = std::min(availableWidth, kStackMaxWidth);
    const int height = sizeHint().height();
    // Bottom-center anchor: the stack hugs the bottom of its parent's central
    // area, with a fixed gutter on either side and a small bottom inset so it
    // sits above the window chrome.
    const int x = (p->width() - width) / 2;
    const int y = p->height() - height - kStackBottomPadding;
    setGeometry(x, std::max(0, y), width, height);
}

void NotificationToastStack::onNotificationAdded(const models::DishNotification& notification) {
    // Cap the visible stack at kMaxVisible. The simplest cap that matches the
    // user's mental model: every new toast pushes out the OLDEST one so the
    // freshest information stays on screen. (dish-android's Snackbar uses a
    // material queue with a single slot; the desktop has the real estate to
    // show three.)
    if (entries_.size() >= kMaxVisible) {
        std::uint64_t oldest = 0;
        // Linear scan: kMaxVisible is 3, so this is fine.
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (oldest == 0 || it.key() < oldest) { oldest = it.key(); }
        }
        if (oldest != 0) { fadeOutAndRemove(oldest); }
    }
    showNotification(notification);
}

void NotificationToastStack::onNotificationDismissed(std::uint64_t id) { fadeOutAndRemove(id); }

void NotificationToastStack::showNotification(const models::DishNotification& notification) {
    auto* label = new QLabel(notification.message, this);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(toastQss(notification.severity));
    if (notification.dismissible) {
        label->setCursor(Qt::PointingHandCursor);
        // Click-to-dismiss: tag the label with its notification id and route
        // the press event through our `eventFilter`. We dismiss locally
        // (fadeOutAndRemove) — the queue is the data-flow surface; the
        // renderer dismisses its own handles to mirror dish-android's
        // `MaterialSnackbarRenderer.SnackbarHandle.dismiss()`.
        label->setProperty("dish_notification_id",
                           QVariant::fromValue<qulonglong>(notification.id));
        label->installEventFilter(this);
    }

    // Fade-in via QGraphicsOpacityEffect — gives a soft entrance instead of a
    // hard pop-in. Effect parent is the label so its lifetime mirrors the
    // label's.
    auto* opacity = new QGraphicsOpacityEffect(label);
    opacity->setOpacity(0.0);
    label->setGraphicsEffect(opacity);

    auto* fadeIn = new QPropertyAnimation(opacity, "opacity", this);
    fadeIn->setDuration(kFadeInMs);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);

    layout_->addWidget(label);
    setVisible(true);
    raise();
    reanchor();
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

    ToastEntry entry;
    entry.id = notification.id;
    entry.widget = label;
    entry.opacity = opacity;
    entry.fadeIn = fadeIn;

    if (notification.durationMs > 0) {
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(notification.durationMs);
        const auto id = notification.id;
        QObject::connect(timer, &QTimer::timeout, this, [this, id] { fadeOutAndRemove(id); });
        timer->start();
        entry.autoDismiss = timer;
    }

    entries_.insert(notification.id, entry);
}

void NotificationToastStack::fadeOutAndRemove(std::uint64_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) { return; }
    ToastEntry entry = it.value();
    entries_.erase(it);

    if (entry.autoDismiss != nullptr) { entry.autoDismiss->stop(); }
    auto* label = entry.widget.data();
    if (label == nullptr) { return; }

    auto* opacity = entry.opacity.data();
    if (opacity == nullptr) {
        label->deleteLater();
    } else {
        auto* fadeOut = new QPropertyAnimation(opacity, "opacity", this);
        fadeOut->setDuration(kFadeOutMs);
        fadeOut->setStartValue(opacity->opacity());
        fadeOut->setEndValue(0.0);
        QObject::connect(fadeOut, &QPropertyAnimation::finished, label, [label, this] {
            // Drop the widget — the next reanchor() collapses the layout
            // so the remaining toasts shift down into the freed slot.
            label->deleteLater();
            reanchor();
            if (entries_.isEmpty()) { setVisible(false); }
        });
        fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

} // namespace dish::ui
