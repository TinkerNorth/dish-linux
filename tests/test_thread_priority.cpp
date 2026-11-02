// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The hot threads ask rtkit, or inside a Flatpak the realtime portal, to raise them as they start.
// A runner cannot grant a priority, so these check the request: which roles ask, the shape of the
// call each broker gets, that a thread asks for itself, and that a refusal changes nothing and is
// logged once. RLIMIT_RTTIME is capped the way rtkit requires before it grants anything.

#include "source/system/ThreadPriority.h"

#include "core/thread/TrackedMutex.h"

#include <catch2/catch_test_macros.hpp>

#include <QDBusMessage>
#include <QString>
#include <QtGlobal>

#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

using dish::source::capRealtimeTime;
using dish::source::highPriorityRequest;
using dish::source::kHighPriorityNice;
using dish::source::kRealtimeTimeLimitUsec;
using dish::source::PriorityBroker;
using dish::source::PriorityBus;
using dish::source::PriorityRequest;
using dish::source::PriorityRequester;
using dish::source::priorityRequestFor;
using dish::thread::ThreadRole;

namespace {

// Records every request and answers each with `refusal`, nothing meaning granted.
class FakeBus final : public PriorityBus {
  public:
    std::optional<QString> send(PriorityBroker broker, const QDBusMessage& request) override {
        brokers.push_back(broker);
        requests.push_back(request);
        return refusal;
    }

    std::optional<QString> refusal;
    std::vector<PriorityBroker> brokers;
    std::vector<QDBusMessage> requests;
};

std::atomic<int> priorityLogLines{0};

void countPriorityLines(QtMsgType /*type*/, const QMessageLogContext& context,
                        const QString& /*message*/) {
    if (context.category != nullptr && std::string_view(context.category) == "dish.priority") {
        priorityLogLines.fetch_add(1);
    }
}

} // namespace

TEST_CASE("the input, USB reader and receive threads ask for priority, and nothing else does",
          "[thread-priority]") {
    CHECK(priorityRequestFor(ThreadRole::SdlInput) == PriorityRequest::High);
    CHECK(priorityRequestFor(ThreadRole::UsbReader) == PriorityRequest::High);
    CHECK(priorityRequestFor(ThreadRole::Receive) == PriorityRequest::High);
    for (const ThreadRole role :
         {ThreadRole::Gui, ThreadRole::Heartbeat, ThreadRole::Playout, ThreadRole::AudioCapture,
          ThreadRole::MoonlightRx, ThreadRole::Update, ThreadRole::Other}) {
        CHECK(priorityRequestFor(role) == PriorityRequest::None);
    }
}

TEST_CASE("rtkit is asked for the thread's nice level on its own object", "[thread-priority]") {
    const QDBusMessage call =
        highPriorityRequest(PriorityBroker::RealtimeKit, 4242, 4243, kHighPriorityNice);

    CHECK(call.type() == QDBusMessage::MethodCallMessage);
    CHECK(call.service() == QStringLiteral("org.freedesktop.RealtimeKit1"));
    CHECK(call.path() == QStringLiteral("/org/freedesktop/RealtimeKit1"));
    CHECK(call.interface() == QStringLiteral("org.freedesktop.RealtimeKit1"));
    CHECK(call.member() == QStringLiteral("MakeThreadHighPriority"));
    const auto args = call.arguments();
    REQUIRE(args.size() == 2);
    CHECK(args[0].metaType() == QMetaType::fromType<qulonglong>());
    CHECK(args[0].toULongLong() == 4243U);
    CHECK(args[1].metaType() == QMetaType::fromType<int>());
    CHECK(args[1].toInt() == -10);
}

TEST_CASE("inside a Flatpak the realtime portal is asked, with the process and the thread",
          "[thread-priority]") {
    const QDBusMessage call =
        highPriorityRequest(PriorityBroker::Portal, 4242, 4243, kHighPriorityNice);

    CHECK(call.service() == QStringLiteral("org.freedesktop.portal.Desktop"));
    CHECK(call.path() == QStringLiteral("/org/freedesktop/portal/desktop"));
    CHECK(call.interface() == QStringLiteral("org.freedesktop.portal.Realtime"));
    CHECK(call.member() == QStringLiteral("MakeThreadHighPriorityWithPID"));
    const auto args = call.arguments();
    REQUIRE(args.size() == 3);
    CHECK(args[0].metaType() == QMetaType::fromType<qulonglong>());
    CHECK(args[0].toULongLong() == 4242U);
    CHECK(args[1].toULongLong() == 4243U);
    CHECK(args[2].metaType() == QMetaType::fromType<int>());
    CHECK(args[2].toInt() == -10);
}

TEST_CASE("a hot thread asks once, for itself, through the broker it was given",
          "[thread-priority]") {
    FakeBus bus;
    const PriorityRequester requester(bus, PriorityBroker::RealtimeKit);
    std::uint64_t asker = 0;
    bool granted = false;

    std::thread hot([&] {
        asker = static_cast<std::uint64_t>(::gettid());
        granted = requester.request(ThreadRole::SdlInput);
    });
    hot.join();

    CHECK(granted);
    REQUIRE(bus.requests.size() == 1U);
    CHECK(bus.brokers[0] == PriorityBroker::RealtimeKit);
    CHECK(bus.requests[0].arguments()[0].toULongLong() == asker);
    CHECK(bus.requests[0].arguments()[1].toInt() == kHighPriorityNice);
}

TEST_CASE("a thread that asks for nothing sends nothing", "[thread-priority]") {
    FakeBus bus;
    const PriorityRequester requester(bus, PriorityBroker::RealtimeKit);

    CHECK(requester.request(ThreadRole::Heartbeat));
    CHECK(requester.request(ThreadRole::Gui));

    CHECK(bus.requests.empty());
}

TEST_CASE("a refusal changes nothing and is logged once", "[thread-priority]") {
    FakeBus bus;
    bus.refusal = QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown: no rtkit");
    const PriorityRequester requester(bus, PriorityBroker::RealtimeKit);
    priorityLogLines.store(0);
    const QtMessageHandler previous = qInstallMessageHandler(countPriorityLines);

    const bool first = requester.request(ThreadRole::SdlInput);
    const bool second = requester.request(ThreadRole::Receive);

    qInstallMessageHandler(previous);
    CHECK_FALSE(first);
    CHECK_FALSE(second);
    CHECK(bus.requests.size() == 2U);
    CHECK(requester.refusalLogged());
    CHECK(priorityLogLines.load() == 1);
}

TEST_CASE("RLIMIT_RTTIME is capped at rtkit's maximum before anything is asked",
          "[thread-priority]") {
    REQUIRE(capRealtimeTime());
    rlimit limit{};
    REQUIRE(::getrlimit(RLIMIT_RTTIME, &limit) == 0);
    CHECK(limit.rlim_cur <= kRealtimeTimeLimitUsec);
    CHECK(limit.rlim_max <= kRealtimeTimeLimitUsec);
    // Capped already: a second call keeps it and still succeeds.
    CHECK(capRealtimeTime());
}
