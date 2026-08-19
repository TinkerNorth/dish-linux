// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The update reducer. Effect lists are asserted as ORDERED vectors because the
// checker executes them in sequence.
//
// Two rules get the most cases, because getting either wrong ships a client
// that nags forever or never: ordering is by the parsed triple only (no wall
// clock anywhere), and "checks off" means off from every phase with no timer
// re-armed.

#include "core/reducer/UpdateMachine.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <vector>

using dish::reducer::backoffDelayMs;
using dish::reducer::jitteredDelayMs;
using dish::reducer::kBackoffBaseMs;
using dish::reducer::kBackoffCapMs;
using dish::reducer::kPeriodicIntervalMs;
using dish::reducer::kReconnectCheckDelayMs;
using dish::reducer::kStartupDelayMs;
using dish::reducer::reduceUpdate;
using dish::reducer::UpdateEffect;
using dish::reducer::UpdateError;
using dish::reducer::UpdateNotice;
using dish::reducer::UpdatePhase;
using dish::reducer::UpdateStatus;
using dish::reducer::UpdateTrigger;
using dish::update::UpdateManifest;
namespace uev = dish::reducer::update_event;
namespace ufx = dish::reducer::update_effect;

namespace {

UpdateManifest manifest(const QString& version, const QString& minimum = QStringLiteral("0.1.0")) {
    UpdateManifest m;
    m.schema = 1;
    m.product = QStringLiteral("dish-linux");
    m.version = version;
    m.channel = QStringLiteral("stable");
    m.publishedAt = QStringLiteral("2026-08-03T14:21:07Z");
    m.minimumSupportedVersion = minimum;
    m.releaseNotesUrl =
        QStringLiteral("https://github.com/TinkerNorth/dish-linux/releases/tag/") + version;
    return m;
}

UpdateStatus atVersion(const QString& current) {
    UpdateStatus s;
    s.currentVersion = current;
    return s;
}

UpdateStatus checking(const QString& current) {
    UpdateStatus s = atVersion(current);
    s.phase = UpdatePhase::Checking;
    return s;
}

template <class T> bool has(const std::vector<UpdateEffect>& fx) {
    for (const auto& e : fx) {
        if (std::get_if<T>(&e) != nullptr) { return true; }
    }
    return false;
}

template <class T> const T* find(const std::vector<UpdateEffect>& fx) {
    for (const auto& e : fx) {
        if (const auto* t = std::get_if<T>(&e)) { return t; }
    }
    return nullptr;
}

} // namespace

TEST_CASE("a check moves Idle to Checking and asks for the manifest", "[update][machine]") {
    const auto r = reduceUpdate(atVersion(QStringLiteral("0.1.0")),
                                uev::CheckRequested{UpdateTrigger::Startup});
    CHECK(r.next.phase == UpdatePhase::Checking);
    REQUIRE(r.effects.size() == 1);
    const auto* fetch = find<ufx::FetchManifest>(r.effects);
    REQUIRE(fetch != nullptr);
    CHECK_FALSE(fetch->manual);
}

TEST_CASE("only a manual trigger marks the fetch manual", "[update][machine]") {
    const auto r = reduceUpdate(atVersion(QStringLiteral("0.1.0")),
                                uev::CheckRequested{UpdateTrigger::Manual});
    const auto* fetch = find<ufx::FetchManifest>(r.effects);
    REQUIRE(fetch != nullptr);
    CHECK(fetch->manual);
}

TEST_CASE("a second check while one is in flight is dropped", "[update][machine]") {
    const auto r = reduceUpdate(checking(QStringLiteral("0.1.0")),
                                uev::CheckRequested{UpdateTrigger::Periodic});
    CHECK(r.next.phase == UpdatePhase::Checking);
    CHECK(r.effects.empty());
}

TEST_CASE("the reachability gate answers without touching the network", "[update][machine]") {
    UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
    s.online = false;
    const auto r = reduceUpdate(s, uev::CheckRequested{UpdateTrigger::Periodic});
    CHECK(r.next.phase == UpdatePhase::Failed);
    CHECK(r.next.error == UpdateError::Offline);
    CHECK_FALSE(has<ufx::FetchManifest>(r.effects));
    CHECK(has<ufx::ScheduleNextCheck>(r.effects));
}

TEST_CASE("a newer release becomes Available and notifies once", "[update][machine]") {
    const auto r = reduceUpdate(checking(QStringLiteral("0.1.0")),
                                uev::ManifestArrived{manifest(QStringLiteral("0.2.0"))});
    CHECK(r.next.phase == UpdatePhase::Available);
    CHECK(r.next.availableVersion == QStringLiteral("0.2.0"));
    CHECK(r.next.notesUrl.endsWith(QStringLiteral("0.2.0")));
    CHECK(r.next.consecutiveFailures == 0);

    const auto* notify = find<ufx::Notify>(r.effects);
    REQUIRE(notify != nullptr);
    CHECK(notify->notice == UpdateNotice::Available);
    CHECK(notify->version == QStringLiteral("0.2.0"));

    const auto* next = find<ufx::ScheduleNextCheck>(r.effects);
    REQUIRE(next != nullptr);
    CHECK(next->delayMs == kPeriodicIntervalMs);
    CHECK(has<ufx::PersistLastCheck>(r.effects));
}

TEST_CASE("the same or an older release is UpToDate", "[update][machine]") {
    SECTION("identical") {
        const auto r = reduceUpdate(checking(QStringLiteral("0.2.0")),
                                    uev::ManifestArrived{manifest(QStringLiteral("0.2.0"))});
        CHECK(r.next.phase == UpdatePhase::UpToDate);
        CHECK(r.next.availableVersion.isEmpty());
        CHECK_FALSE(has<ufx::Notify>(r.effects));
    }
    SECTION("older, which is a yank the client must not roll back to") {
        const auto r = reduceUpdate(checking(QStringLiteral("0.3.0")),
                                    uev::ManifestArrived{manifest(QStringLiteral("0.2.0"))});
        CHECK(r.next.phase == UpdatePhase::UpToDate);
        CHECK(r.next.availableVersion.isEmpty());
    }
}

TEST_CASE("ordering is by the triple, never by publishedAt", "[update][machine]") {
    UpdateManifest older = manifest(QStringLiteral("0.1.9"));
    older.publishedAt = QStringLiteral("2099-01-01T00:00:00Z");
    const auto r = reduceUpdate(checking(QStringLiteral("0.2.0")), uev::ManifestArrived{older});
    CHECK(r.next.phase == UpdatePhase::UpToDate);
}

TEST_CASE("a skipped version stays muted while the manifest still offers it", "[update][machine]") {
    UpdateStatus s = checking(QStringLiteral("0.1.0"));
    s.skippedVersion = QStringLiteral("0.2.0");
    const auto r = reduceUpdate(s, uev::ManifestArrived{manifest(QStringLiteral("0.2.0"))});
    CHECK(r.next.phase == UpdatePhase::UpToDate);
    CHECK_FALSE(has<ufx::Notify>(r.effects));
}

TEST_CASE("a newer release than the skipped one is offered again", "[update][machine]") {
    UpdateStatus s = checking(QStringLiteral("0.1.0"));
    s.skippedVersion = QStringLiteral("0.2.0");
    const auto r = reduceUpdate(s, uev::ManifestArrived{manifest(QStringLiteral("0.3.0"))});
    CHECK(r.next.phase == UpdatePhase::Available);
    CHECK(r.next.availableVersion == QStringLiteral("0.3.0"));
}

TEST_CASE("a required update overrides a skip and notifies Unsupported first",
          "[update][machine]") {
    UpdateStatus s = checking(QStringLiteral("0.1.0"));
    s.skippedVersion = QStringLiteral("0.2.0");
    const auto r = reduceUpdate(
        s, uev::ManifestArrived{manifest(QStringLiteral("0.2.0"), QStringLiteral("0.2.0"))});
    CHECK(r.next.required);
    CHECK(r.next.phase == UpdatePhase::Available);

    // Unsupported precedes Available: the reason to act is stated before the
    // thing to act on.
    std::vector<UpdateNotice> notices;
    for (const auto& e : r.effects) {
        if (const auto* n = std::get_if<ufx::Notify>(&e)) { notices.push_back(n->notice); }
    }
    REQUIRE(notices.size() == 2);
    CHECK(notices[0] == UpdateNotice::Unsupported);
    CHECK(notices[1] == UpdateNotice::Available);
}

TEST_CASE("a required update cannot be skipped", "[update][machine]") {
    UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
    s.phase = UpdatePhase::Available;
    s.availableVersion = QStringLiteral("0.2.0");
    s.required = true;
    const auto r = reduceUpdate(s, uev::SkipRequested{QStringLiteral("0.2.0")});
    CHECK(r.next.phase == UpdatePhase::Available);
    CHECK(r.next.skippedVersion.isEmpty());
}

TEST_CASE("skipping the offered version clears the offer", "[update][machine]") {
    UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
    s.phase = UpdatePhase::Available;
    s.availableVersion = QStringLiteral("0.2.0");
    s.notesUrl = QStringLiteral("https://example.invalid");
    const auto r = reduceUpdate(s, uev::SkipRequested{QStringLiteral("0.2.0")});
    CHECK(r.next.phase == UpdatePhase::UpToDate);
    CHECK(r.next.skippedVersion == QStringLiteral("0.2.0"));
    CHECK(r.next.availableVersion.isEmpty());
    CHECK(r.next.notesUrl.isEmpty());
}

TEST_CASE("a failed check climbs the backoff ladder", "[update][machine]") {
    UpdateStatus s = checking(QStringLiteral("0.1.0"));
    const auto first = reduceUpdate(s, uev::CheckFailed{UpdateError::Http});
    CHECK(first.next.phase == UpdatePhase::Failed);
    CHECK(first.next.error == UpdateError::Http);
    CHECK(first.next.consecutiveFailures == 1);
    const auto* d1 = find<ufx::ScheduleNextCheck>(first.effects);
    REQUIRE(d1 != nullptr);
    CHECK(d1->delayMs == kBackoffBaseMs);

    UpdateStatus again = first.next;
    again.phase = UpdatePhase::Checking;
    const auto second = reduceUpdate(again, uev::CheckFailed{UpdateError::Http});
    CHECK(second.next.consecutiveFailures == 2);
    const auto* d2 = find<ufx::ScheduleNextCheck>(second.effects);
    REQUIRE(d2 != nullptr);
    CHECK(d2->delayMs == kBackoffBaseMs * 2);
}

TEST_CASE("the backoff ladder doubles and then caps", "[update][machine]") {
    CHECK(backoffDelayMs(0) == kBackoffBaseMs);
    CHECK(backoffDelayMs(1) == kBackoffBaseMs);
    CHECK(backoffDelayMs(2) == kBackoffBaseMs * 2);
    CHECK(backoffDelayMs(3) == kBackoffBaseMs * 4);
    CHECK(backoffDelayMs(99) == kBackoffCapMs);
}

TEST_CASE("jitter is deterministic in its draw and centred on the base", "[update][machine]") {
    CHECK(jitteredDelayMs(1000, 0.5) == 1000);
    CHECK(jitteredDelayMs(1000, 0.0) == 800);
    CHECK(jitteredDelayMs(1000, 1.0) == 1200);
    // Out-of-range draws clamp rather than extrapolate.
    CHECK(jitteredDelayMs(1000, -1.0) == 800);
    CHECK(jitteredDelayMs(1000, 2.0) == 1200);
}

TEST_CASE("a manifest that arrives outside Checking is ignored", "[update][machine]") {
    UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
    s.phase = UpdatePhase::UpToDate;
    const auto r = reduceUpdate(s, uev::ManifestArrived{manifest(QStringLiteral("0.9.0"))});
    CHECK(r.next.phase == UpdatePhase::UpToDate);
    CHECK(r.next.availableVersion.isEmpty());
    CHECK(r.effects.empty());
}

TEST_CASE("checks off means off from every phase, with no timer re-armed", "[update][machine]") {
    for (const UpdatePhase phase : {UpdatePhase::Idle, UpdatePhase::Checking, UpdatePhase::UpToDate,
                                    UpdatePhase::Available, UpdatePhase::Failed}) {
        UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
        s.phase = phase;
        s.error = UpdateError::Http;
        const auto r = reduceUpdate(s, uev::PrefsChanged{/*checksEnabled=*/false, QString()});
        CHECK(r.next.phase == UpdatePhase::Disabled);
        CHECK(r.next.error == UpdateError::None);
        CHECK_FALSE(has<ufx::ScheduleNextCheck>(r.effects));
        CHECK_FALSE(has<ufx::FetchManifest>(r.effects));
    }
}

TEST_CASE("re-enabling checks behaves like a cold start", "[update][machine]") {
    UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
    s.phase = UpdatePhase::Disabled;
    s.checksEnabled = false;
    const auto r = reduceUpdate(s, uev::PrefsChanged{/*checksEnabled=*/true, QString()});
    CHECK(r.next.phase == UpdatePhase::Idle);
    const auto* next = find<ufx::ScheduleNextCheck>(r.effects);
    REQUIRE(next != nullptr);
    CHECK(next->delayMs == kStartupDelayMs);
}

TEST_CASE("coming back online re-arms only an offline-gated failure", "[update][machine]") {
    SECTION("offline failure re-arms on the reconnect delay") {
        UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
        s.phase = UpdatePhase::Failed;
        s.error = UpdateError::Offline;
        s.online = false;
        const auto r = reduceUpdate(s, uev::ReachabilityChanged{true});
        CHECK(r.next.online);
        const auto* next = find<ufx::ScheduleNextCheck>(r.effects);
        REQUIRE(next != nullptr);
        CHECK(next->delayMs == kReconnectCheckDelayMs);
    }
    SECTION("an HTTP failure keeps its own backoff") {
        UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
        s.phase = UpdatePhase::Failed;
        s.error = UpdateError::Http;
        s.online = false;
        const auto r = reduceUpdate(s, uev::ReachabilityChanged{true});
        CHECK_FALSE(has<ufx::ScheduleNextCheck>(r.effects));
    }
    SECTION("a disabled client never re-arms") {
        UpdateStatus s = atVersion(QStringLiteral("0.1.0"));
        s.phase = UpdatePhase::Failed;
        s.error = UpdateError::Offline;
        s.checksEnabled = false;
        const auto r = reduceUpdate(s, uev::ReachabilityChanged{true});
        CHECK_FALSE(has<ufx::ScheduleNextCheck>(r.effects));
    }
}
