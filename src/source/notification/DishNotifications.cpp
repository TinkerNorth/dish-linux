// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/notification/DishNotifications.h"

namespace dish::source {

DishNotifications::DishNotifications(QObject* parent) : QObject(parent) {}

int DishNotifications::defaultDurationForSeverity(models::DishNotification::Severity severity) {
    switch (severity) {
    case models::DishNotification::Severity::Info:
    case models::DishNotification::Severity::Success:
        return models::DishNotification::kDurationShortMs;
    case models::DishNotification::Severity::Warn:
    case models::DishNotification::Severity::Error:
        return models::DishNotification::kDurationLongMs;
    }
    return models::DishNotification::kDurationShortMs;
}

int DishNotifications::post(models::DishNotification notification) {
    // Assigned centrally so callers hand us a literal struct without threading a
    // counter through every emit site.
    const int id = nextId_++;
    notification.id = id;
    if (notification.durationMs == kUseSeverityDefault) {
        notification.durationMs = defaultDurationForSeverity(notification.severity);
    }
    posts_.push(notification);
    emit notificationPosted(notification);
    return id;
}

int DishNotifications::postError(const QString& message) {
    models::DishNotification n;
    n.severity = models::DishNotification::Severity::Error;
    n.kind = QStringLiteral("error");
    n.message = message;
    n.durationMs = kUseSeverityDefault;
    return post(std::move(n));
}

void DishNotifications::dismiss(int id) {
    dismissals_.push(id);
    emit notificationDismissed(id);
}

} // namespace dish::source
