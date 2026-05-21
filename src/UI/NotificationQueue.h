// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/DishNotification.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <cstdint>

namespace dish::ui {

// Process-scoped notification bus. Mirrors dish-android's `DishNotifications`
// — emitters call `post(...) / `info(...)` / `error(...)` to enqueue a
// `models::DishNotification`; observers (the toast stack widget today, future
// system-tray notifier, tests) react to `notificationAdded` and
// `notificationDismissed`.
//
// Threading: posts must be issued on the Qt main thread (the queue mutates
// QHash state and emits queued signals). Same constraint the existing
// `ErrorBanner` already imposes via its parent QObject — every emitter on
// the Linux client is already main-thread.
//
// Same-key replacement: a post with a non-empty `key` first dismisses the
// prior live entry sharing that key. Use for state-driven banners — e.g.
// "satellite isn't responding" must not stack twice if the user bounces the
// connection. Matches dish-android `DishNotifications.handlePost`.
class NotificationQueue : public QObject {
    Q_OBJECT
  public:
    explicit NotificationQueue(QObject* parent = nullptr);
    ~NotificationQueue() override;

    // High-level enqueue. Assigns a monotonic id, fills `defaultDurationFor`
    // when `durationMs == -1`, applies same-key replacement, then emits
    // `notificationAdded(notification)`. Returns the assigned id so the
    // emitter can later `dismiss(id)` (e.g. when the underlying state clears
    // before the auto-dismiss fires).
    std::uint64_t post(models::NotificationSeverity severity, const QString& message,
                       models::NotificationKind kind = models::NotificationKind::Generic,
                       const QString& key = QString(), int durationMs = -1,
                       bool dismissible = true);

    // Convenience wrappers — keep call sites readable.
    std::uint64_t info(const QString& message,
                       models::NotificationKind kind = models::NotificationKind::Generic,
                       const QString& key = QString());
    std::uint64_t success(const QString& message,
                          models::NotificationKind kind = models::NotificationKind::Generic,
                          const QString& key = QString());
    std::uint64_t warn(const QString& message,
                       models::NotificationKind kind = models::NotificationKind::Generic,
                       const QString& key = QString());
    std::uint64_t error(const QString& message,
                        models::NotificationKind kind = models::NotificationKind::Generic,
                        const QString& key = QString());

    // Dismiss a previously-posted notification by id. No-op if already gone
    // (auto-dismissed or user-dismissed). Emits `notificationDismissed(id)`.
    void dismiss(std::uint64_t id);

  signals:
    // Fired after a successful `post(...)`. Observers should react by showing
    // the toast or feeding their own UI surface.
    void notificationAdded(const models::DishNotification& notification);

    // Fired by `dismiss(id)`. Observers should react by removing the matching
    // toast. The id matches a prior `notificationAdded`'s `.id`.
    void notificationDismissed(std::uint64_t id);

  private:
    std::uint64_t nextId_ = 1;
    // Side index: key → id for the currently-live notification with that key.
    // On every same-key post we look up the prior id, emit a dismissal and
    // overwrite. Dismissals erase from this map; auto-dismiss in the renderer
    // calls back through `dismiss(id)` and lands in the same code path.
    QHash<QString, std::uint64_t> liveByKey_;
};

} // namespace dish::ui
