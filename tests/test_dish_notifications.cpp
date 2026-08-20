// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The same-kind dedup is not here: it lives in the read-only renderer.

#include "source/notification/DishNotifications.h"
#include "source/notification/EventChannel.h"

#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QString>

#include <string>
#include <vector>

using dish::models::DishNotification;
using dish::source::DishNotifications;
using dish::source::EventChannel;

namespace {

DishNotification make(DishNotification::Severity sev, const QString& kind,
                      int durationMs = DishNotifications::kUseSeverityDefault) {
    DishNotification n;
    n.severity = sev;
    n.kind = kind;
    n.message = QStringLiteral("msg");
    n.durationMs = durationMs;
    return n;
}

} // namespace

TEST_CASE("DishNotifications: post assigns monotonically increasing ids", "[notify]") {
    DishNotifications dn;
    const int a = dn.post(make(DishNotification::Severity::Info, "a"));
    const int b = dn.post(make(DishNotification::Severity::Info, "b"));
    const int c = dn.post(make(DishNotification::Severity::Info, "c"));
    REQUIRE(a == 1);
    REQUIRE(b == 2);
    REQUIRE(c == 3);
}

TEST_CASE("DishNotifications: ids are never reused after a dismiss", "[notify]") {
    DishNotifications dn;
    const int a = dn.post(make(DishNotification::Severity::Info, "a"));
    dn.dismiss(a);
    const int b = dn.post(make(DishNotification::Severity::Info, "a")); // same kind
    REQUIRE(b == a + 1);
}

TEST_CASE("DishNotifications: severity maps to the default duration", "[notify]") {
    REQUIRE(DishNotifications::defaultDurationForSeverity(DishNotification::Severity::Info) ==
            DishNotification::kDurationShortMs);
    REQUIRE(DishNotifications::defaultDurationForSeverity(DishNotification::Severity::Success) ==
            DishNotification::kDurationShortMs);
    REQUIRE(DishNotifications::defaultDurationForSeverity(DishNotification::Severity::Warn) ==
            DishNotification::kDurationLongMs);
    REQUIRE(DishNotifications::defaultDurationForSeverity(DishNotification::Severity::Error) ==
            DishNotification::kDurationLongMs);
}

TEST_CASE("DishNotifications: a post with the sentinel resolves the severity default", "[notify]") {
    DishNotifications dn;
    std::vector<DishNotification> seen;
    dn.posts().subscribe([&](const DishNotification& n) { seen.push_back(n); });

    dn.post(make(DishNotification::Severity::Info, "i"));
    dn.post(make(DishNotification::Severity::Error, "e"));

    REQUIRE(seen.size() == 2);
    REQUIRE(seen[0].durationMs == DishNotification::kDurationShortMs);
    REQUIRE(seen[1].durationMs == DishNotification::kDurationLongMs);
}

TEST_CASE("DishNotifications: an explicit duration overrides the severity default", "[notify]") {
    DishNotifications dn;
    std::vector<DishNotification> seen;
    dn.posts().subscribe([&](const DishNotification& n) { seen.push_back(n); });
    dn.post(make(DishNotification::Severity::Error, "e", /*durationMs=*/1500));
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].durationMs == 1500);
}

TEST_CASE("DishNotifications: a persistent (0) duration is preserved", "[notify]") {
    DishNotifications dn;
    std::vector<DishNotification> seen;
    dn.posts().subscribe([&](const DishNotification& n) { seen.push_back(n); });
    dn.post(make(DishNotification::Severity::Warn, "w",
                 /*durationMs=*/DishNotification::kDurationPersistent));
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].durationMs == DishNotification::kDurationPersistent);
}

TEST_CASE("DishNotifications: the posted struct carries its assigned id", "[notify]") {
    DishNotifications dn;
    std::vector<DishNotification> seen;
    dn.posts().subscribe([&](const DishNotification& n) { seen.push_back(n); });
    const int id = dn.post(make(DishNotification::Severity::Info, "i"));
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].id == id);
    REQUIRE(seen[0].kind == QStringLiteral("i"));
}

TEST_CASE("DishNotifications: postError emits an Error with the long duration", "[notify]") {
    DishNotifications dn;
    std::vector<DishNotification> seen;
    dn.posts().subscribe([&](const DishNotification& n) { seen.push_back(n); });
    dn.postError(QStringLiteral("boom"));
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].severity == DishNotification::Severity::Error);
    REQUIRE(seen[0].message == QStringLiteral("boom"));
    REQUIRE(seen[0].durationMs == DishNotification::kDurationLongMs);
}

TEST_CASE("DishNotifications: dismiss emits the id on the dismissal channel", "[notify]") {
    DishNotifications dn;
    std::vector<int> dismissed;
    dn.dismissals().subscribe([&](const int& id) { dismissed.push_back(id); });
    const int id = dn.post(make(DishNotification::Severity::Info, "i"));
    dn.dismiss(id);
    REQUIRE(dismissed.size() == 1);
    REQUIRE(dismissed[0] == id);
}

TEST_CASE("DishNotifications: dismissing an unknown id still forwards (renderer no-ops)",
          "[notify]") {
    DishNotifications dn;
    std::vector<int> dismissed;
    dn.dismissals().subscribe([&](const int& id) { dismissed.push_back(id); });
    dn.dismiss(999);
    REQUIRE(dismissed.size() == 1);
    REQUIRE(dismissed[0] == 999);
}

TEST_CASE("DishNotifications: emits Qt signals mirroring the channels", "[notify]") {
    DishNotifications dn;
    int postedCount = 0;
    int dismissedId = -1;
    int dismissedCount = 0;
    QObject::connect(&dn, &DishNotifications::notificationPosted, &dn,
                     [&](const DishNotification&) { ++postedCount; });
    QObject::connect(&dn, &DishNotifications::notificationDismissed, &dn, [&](int id) {
        ++dismissedCount;
        dismissedId = id;
    });
    const int id = dn.post(make(DishNotification::Severity::Info, "i"));
    dn.dismiss(id);
    REQUIRE(postedCount == 1);
    REQUIRE(dismissedCount == 1);
    REQUIRE(dismissedId == id);
}

TEST_CASE("EventChannel: delivers live when a listener is attached", "[notify]") {
    EventChannel<int> ch(4);
    std::vector<int> seen;
    ch.subscribe([&](const int& v) { seen.push_back(v); });
    ch.push(1);
    ch.push(2);
    REQUIRE(seen == std::vector<int>{1, 2});
    REQUIRE(ch.buffered() == 0);
}

TEST_CASE("EventChannel: buffers when no listener, then drains FIFO on subscribe", "[notify]") {
    EventChannel<int> ch(8);
    ch.push(10);
    ch.push(20);
    ch.push(30);
    REQUIRE(ch.buffered() == 3);
    std::vector<int> seen;
    ch.subscribe([&](const int& v) { seen.push_back(v); });
    REQUIRE(seen == std::vector<int>{10, 20, 30});
    REQUIRE(ch.buffered() == 0);
}

TEST_CASE("EventChannel: DROP_OLDEST discards the oldest past capacity", "[notify]") {
    EventChannel<int> ch(3);
    ch.push(1);
    ch.push(2);
    ch.push(3);
    ch.push(4); // overflow: drops 1
    ch.push(5); // overflow: drops 2
    REQUIRE(ch.buffered() == 3);
    std::vector<int> seen;
    ch.subscribe([&](const int& v) { seen.push_back(v); });
    REQUIRE(seen == std::vector<int>{3, 4, 5});
}

TEST_CASE("DishNotifications: posts buffered before a renderer attaches drain to it", "[notify]") {
    DishNotifications dn;
    dn.post(make(DishNotification::Severity::Error, "e1"));
    dn.post(make(DishNotification::Severity::Error, "e2"));
    dn.post(make(DishNotification::Severity::Error, "e3"));
    REQUIRE(dn.posts().buffered() == 3);

    std::vector<int> ids;
    dn.posts().subscribe([&](const DishNotification& n) { ids.push_back(n.id); });
    REQUIRE(ids == std::vector<int>{1, 2, 3});
}

TEST_CASE("DishNotifications: the post channel honours DROP_OLDEST capacity", "[notify]") {
    DishNotifications dn;
    const int over = DishNotifications::kChannelCapacity + 2;
    for (int i = 0; i < over; ++i) {
        dn.post(make(DishNotification::Severity::Info, QStringLiteral("k")));
    }
    REQUIRE(static_cast<int>(dn.posts().buffered()) == DishNotifications::kChannelCapacity);

    std::vector<int> ids;
    dn.posts().subscribe([&](const DishNotification& n) { ids.push_back(n.id); });
    // Ids 1 and 2 were dropped, so the backlog starts at 3.
    REQUIRE(static_cast<int>(ids.size()) == DishNotifications::kChannelCapacity);
    REQUIRE(ids.front() == 3);
    REQUIRE(ids.back() == over);
}

TEST_CASE("EventChannel: unsubscribe re-enables buffering", "[notify]") {
    EventChannel<int> ch(4);
    std::vector<int> seen;
    ch.subscribe([&](const int& v) { seen.push_back(v); });
    ch.push(1);
    ch.unsubscribe();
    ch.push(2); // buffered now
    ch.push(3);
    REQUIRE(seen == std::vector<int>{1});
    REQUIRE(ch.buffered() == 2);
}

TEST_CASE("EventChannel: a later subscribe replaces the prior listener", "[notify]") {
    EventChannel<int> ch(4);
    std::vector<int> first;
    std::vector<int> second;
    ch.subscribe([&](const int& v) { first.push_back(v); });
    ch.push(1);
    ch.subscribe([&](const int& v) { second.push_back(v); });
    ch.push(2);
    REQUIRE(first == std::vector<int>{1});
    REQUIRE(second == std::vector<int>{2});
}

TEST_CASE("EventChannel: zero capacity is clamped to one", "[notify]") {
    EventChannel<int> ch(0);
    REQUIRE(ch.capacity() == 1);
    ch.push(1);
    ch.push(2); // drops 1
    REQUIRE(ch.buffered() == 1);
    std::vector<int> seen;
    ch.subscribe([&](const int& v) { seen.push_back(v); });
    REQUIRE(seen == std::vector<int>{2});
}
