// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// DishNotifications — the in-app notification post/dismiss bus: two bounded
// DROP_OLDEST event channels, monotonic ids, and severity->duration defaults.
// The renderer above it owns same-kind dedup and dismiss-on-action.
//
// One of only two event-emitting classes in the design (the other is the
// connection manager); Sources otherwise expose state, not events. Both the raw
// channels and Qt signals carry the same value, so non-UI consumers and the
// renderer see one truth.
//
// Id assignment and duration defaults live HERE, not in the renderer. The
// DROP_OLDEST overflow bounds a poster that outruns a detached renderer.

#pragma once

#include "Models/Models.h"
#include "source/notification/EventChannel.h"

#include <QObject>
#include <QString>

namespace dish::source {

class DishNotifications : public QObject {
    Q_OBJECT
  public:
    // Buffer capacity before DROP_OLDEST kicks in.
    static constexpr int kChannelCapacity = 16;

    explicit DishNotifications(QObject* parent = nullptr);

    // Info / Success -> short (3.5 s), Warn / Error -> long (6 s). Static so it
    // is testable without an instance.
    static int defaultDurationForSeverity(models::DishNotification::Severity severity);

    // Sentinel a caller leaves in durationMs to request the severity default.
    // Distinct from kDurationPersistent == 0, "stay until dismissed".
    static constexpr int kUseSeverityDefault = -1;

    // Returns the assigned id: process-local, a dismiss handle rather than a
    // stable identity.
    int post(models::DishNotification notification);

    int postError(const QString& message);

    void dismiss(int id);

    // The raw channels, for non-widget consumers and tests; the renderer uses
    // the Qt signals below.
    EventChannel<models::DishNotification>& posts() { return posts_; }
    EventChannel<int>& dismissals() { return dismissals_; }

  signals:
    // Carries the id-stamped, duration-resolved notification.
    void notificationPosted(const dish::models::DishNotification& notification);
    void notificationDismissed(int id);

  private:
    int nextId_ = 1;
    EventChannel<models::DishNotification> posts_{kChannelCapacity};
    EventChannel<int> dismissals_{kChannelCapacity};
};

} // namespace dish::source
