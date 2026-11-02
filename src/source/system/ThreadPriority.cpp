// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/ThreadPriority.h"

#include <QDBusConnection>
#include <QLoggingCategory>
#include <QVariant>

#include <sys/resource.h>
#include <unistd.h>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcPriority, "dish.priority", QtInfoMsg)

constexpr int kAnswerTimeoutMs = 250;

} // namespace

QDBusMessage highPriorityRequest(PriorityBroker broker, std::uint64_t pid, std::uint64_t tid,
                                 std::int32_t nice) {
    if (broker == PriorityBroker::Portal) {
        QDBusMessage call =
            QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
                                           QStringLiteral("/org/freedesktop/portal/desktop"),
                                           QStringLiteral("org.freedesktop.portal.Realtime"),
                                           QStringLiteral("MakeThreadHighPriorityWithPID"));
        call << QVariant::fromValue(static_cast<qulonglong>(pid))
             << QVariant::fromValue(static_cast<qulonglong>(tid)) << QVariant::fromValue(nice);
        return call;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.RealtimeKit1"),
        QStringLiteral("/org/freedesktop/RealtimeKit1"),
        QStringLiteral("org.freedesktop.RealtimeKit1"), QStringLiteral("MakeThreadHighPriority"));
    call << QVariant::fromValue(static_cast<qulonglong>(tid)) << QVariant::fromValue(nice);
    return call;
}

std::optional<QString> DbusPriorityBus::send(PriorityBroker broker, const QDBusMessage& request) {
    QDBusConnection bus = broker == PriorityBroker::Portal ? QDBusConnection::sessionBus()
                                                           : QDBusConnection::systemBus();
    if (!bus.isConnected()) { return bus.lastError().message(); }
    const QDBusMessage reply = bus.call(request, QDBus::Block, kAnswerTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return reply.errorName() + QStringLiteral(": ") + reply.errorMessage();
    }
    return std::nullopt;
}

bool capRealtimeTime() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_RTTIME, &limit) != 0) { return false; }
    const auto cap = static_cast<rlim_t>(kRealtimeTimeLimitUsec);
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max <= cap && limit.rlim_cur <= cap) {
        return true;
    }
    limit.rlim_cur = cap;
    limit.rlim_max = cap;
    return ::setrlimit(RLIMIT_RTTIME, &limit) == 0;
}

bool PriorityRequester::request(thread::ThreadRole role) const {
    if (priorityRequestFor(role) == PriorityRequest::None) { return true; }
    capRealtimeTime();
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const auto tid = static_cast<std::uint64_t>(::gettid());
    const auto refused =
        bus_.send(broker_, highPriorityRequest(broker_, pid, tid, kHighPriorityNice));
    if (!refused.has_value()) { return true; }
    if (!refusalLogged_.exchange(true, std::memory_order_relaxed)) {
        qCInfo(lcPriority) << thread::threadRoleName(role)
                           << "keeps its priority; the request was refused:" << *refused;
    }
    return false;
}

} // namespace dish::source
