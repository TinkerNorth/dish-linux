// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for dish::ui::NotificationQueue — the process-scoped bus that fans
// AppModel::errorMessage (and any future emitter) into the toast stack.
// Tests focus on the data-flow surface, not the View work:
//   * post() assigns monotonic ids and emits notificationAdded with the right
//     severity + default duration,
//   * dismiss(id) emits notificationDismissed, idempotent,
//   * same-key replacement dismisses the prior live entry before adding.
//
// Rendering (`NotificationToastStack`) needs a QApplication and is exercised
// by hand from the live binary — the queue contract above is what feeds it.

#include "Models/DishNotification.h"
#include "UI/NotificationQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QString>

using dish::models::DishNotification;
using dish::models::NotificationKind;
using dish::models::NotificationSeverity;
using dish::ui::NotificationQueue;

namespace {

// Pull the first `notificationAdded` argument out of a QSignalSpy as a
// DishNotification — the queue passes the value through a Q_DECLARE_METATYPE-
// less signal so we get a QVariant<DishNotification> on the spy.
DishNotification firstAdded(const QSignalSpy& spy) {
    REQUIRE(spy.count() >= 1);
    return spy.at(0).at(0).value<DishNotification>();
}

} // namespace

TEST_CASE("post assigns monotonic ids starting at 1", "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    const auto a = queue.info(QStringLiteral("first"));
    const auto b = queue.info(QStringLiteral("second"));
    const auto c = queue.error(QStringLiteral("third"));
    REQUIRE(a == 1);
    REQUIRE(b == 2);
    REQUIRE(c == 3);
}

TEST_CASE("post emits notificationAdded with the right severity and message",
          "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    QSignalSpy spy(&queue, &NotificationQueue::notificationAdded);

    queue.warn(QStringLiteral("almost gone"));
    REQUIRE(spy.count() == 1);
    const auto n = firstAdded(spy);
    REQUIRE(n.severity == NotificationSeverity::Warn);
    REQUIRE(n.message == QStringLiteral("almost gone"));
    REQUIRE(n.id == 1);
    REQUIRE(n.dismissible);
    // WARN uses the long duration by default.
    REQUIRE(n.durationMs == DishNotification::kDurationLongMs);
}

TEST_CASE("post default duration tracks severity", "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    QSignalSpy spy(&queue, &NotificationQueue::notificationAdded);

    queue.info(QStringLiteral("hi"));
    queue.success(QStringLiteral("ok"));
    queue.warn(QStringLiteral("watch"));
    queue.error(QStringLiteral("nope"));

    REQUIRE(spy.count() == 4);
    REQUIRE(spy.at(0).at(0).value<DishNotification>().durationMs ==
            DishNotification::kDurationShortMs);
    REQUIRE(spy.at(1).at(0).value<DishNotification>().durationMs ==
            DishNotification::kDurationShortMs);
    REQUIRE(spy.at(2).at(0).value<DishNotification>().durationMs ==
            DishNotification::kDurationLongMs);
    REQUIRE(spy.at(3).at(0).value<DishNotification>().durationMs ==
            DishNotification::kDurationLongMs);
}

TEST_CASE("dismiss emits notificationDismissed with the matching id", "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    QSignalSpy dismissedSpy(&queue, &NotificationQueue::notificationDismissed);

    const auto id = queue.info(QStringLiteral("hi"));
    queue.dismiss(id);

    REQUIRE(dismissedSpy.count() == 1);
    REQUIRE(dismissedSpy.at(0).at(0).toULongLong() == id);
}

TEST_CASE("same-key post dismisses the prior live entry first", "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    QSignalSpy addedSpy(&queue, &NotificationQueue::notificationAdded);
    QSignalSpy dismissedSpy(&queue, &NotificationQueue::notificationDismissed);

    const auto a = queue.post(NotificationSeverity::Warn, QStringLiteral("wifi off"),
                              NotificationKind::System, QStringLiteral("wifi"));
    const auto b = queue.post(NotificationSeverity::Warn, QStringLiteral("still off"),
                              NotificationKind::System, QStringLiteral("wifi"));

    // First post just added; second post dismissed the first then added.
    REQUIRE(addedSpy.count() == 2);
    REQUIRE(dismissedSpy.count() == 1);
    REQUIRE(dismissedSpy.at(0).at(0).toULongLong() == a);
    // Ids are still monotonic.
    REQUIRE(b == a + 1);
}

TEST_CASE("dismiss is a no-op for an unknown id", "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    QSignalSpy dismissedSpy(&queue, &NotificationQueue::notificationDismissed);
    // Unknown id still emits dismissedSpy (one-shot signal) but mutates no
    // internal state — exercising the linear scan path with no match.
    queue.dismiss(99);
    REQUIRE(dismissedSpy.count() == 1);
}

TEST_CASE("explicit durationMs overrides the severity default", "[notificationqueue]") {
    qRegisterMetaType<DishNotification>("dish::models::DishNotification");
    NotificationQueue queue;
    QSignalSpy spy(&queue, &NotificationQueue::notificationAdded);

    queue.post(NotificationSeverity::Error, QStringLiteral("persist"), NotificationKind::Generic,
               QString(), DishNotification::kPersistent);

    const auto n = firstAdded(spy);
    REQUIRE(n.durationMs == DishNotification::kPersistent);
}
