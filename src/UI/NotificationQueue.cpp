// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "NotificationQueue.h"

namespace dish::ui {

NotificationQueue::NotificationQueue(QObject* parent) : QObject(parent) {}

NotificationQueue::~NotificationQueue() = default;

std::uint64_t NotificationQueue::post(models::NotificationSeverity severity, const QString& message,
                                      models::NotificationKind kind, const QString& key,
                                      int durationMs, bool dismissible) {
    // Same-key replacement first: if there's a live entry for `key`, drop it
    // before assigning a new id. Mirrors dish-android `handlePost`.
    if (!key.isEmpty()) {
        auto it = liveByKey_.find(key);
        if (it != liveByKey_.end()) {
            const auto priorId = it.value();
            liveByKey_.erase(it);
            emit notificationDismissed(priorId);
        }
    }

    models::DishNotification n;
    n.id = nextId_++;
    n.kind = kind;
    n.severity = severity;
    n.message = message;
    n.dismissible = dismissible;
    n.durationMs = (durationMs < 0) ? models::defaultDurationFor(severity) : durationMs;

    if (!key.isEmpty()) { liveByKey_.insert(key, n.id); }

    emit notificationAdded(n);
    return n.id;
}

std::uint64_t NotificationQueue::info(const QString& message, models::NotificationKind kind,
                                      const QString& key) {
    return post(models::NotificationSeverity::Info, message, kind, key);
}

std::uint64_t NotificationQueue::success(const QString& message, models::NotificationKind kind,
                                         const QString& key) {
    return post(models::NotificationSeverity::Success, message, kind, key);
}

std::uint64_t NotificationQueue::warn(const QString& message, models::NotificationKind kind,
                                      const QString& key) {
    return post(models::NotificationSeverity::Warn, message, kind, key);
}

std::uint64_t NotificationQueue::error(const QString& message, models::NotificationKind kind,
                                       const QString& key) {
    return post(models::NotificationSeverity::Error, message, kind, key);
}

void NotificationQueue::dismiss(std::uint64_t id) {
    // Drop from the key index if this id owned a key slot. We don't track the
    // reverse direction (id → key) to keep memory tight; the QHash here is
    // expected to hold at most a handful of entries at any given moment, so a
    // linear scan is cheaper than a second map.
    for (auto it = liveByKey_.begin(); it != liveByKey_.end(); ++it) {
        if (it.value() == id) {
            liveByKey_.erase(it);
            break;
        }
    }
    emit notificationDismissed(id);
}

} // namespace dish::ui
