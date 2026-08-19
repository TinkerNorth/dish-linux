// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The checker drives the reducer against a fake manifest gateway, so a whole
// check cycle is assertable with no sockets and no event loop: the fake
// delivers its callback inline on the calling thread.
//
// The schedule is driven through pendingCheckDelayMs()/firePendingCheck()
// rather than a real timer, and the clock is injected, so nothing here waits.

#include "update/UpdateChecker.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <utility>

using dish::reducer::UpdateError;
using dish::reducer::UpdateNotice;
using dish::reducer::UpdatePhase;
using dish::reducer::update_schedule::kPeriodicIntervalMs;
using dish::reducer::update_schedule::kStartupDelayMs;
using dish::source::UpdatePreferenceStore;
using dish::update::ManifestFetchResult;
using dish::update::ManifestGateway;
using dish::update::UpdateChecker;
using dish::update::UpdateManifest;

namespace {

// Records what was asked for and answers only when the test says so, so the
// in-flight window is observable.
class FakeManifestGateway : public ManifestGateway {
  public:
    void fetch(Callback done) override {
        fetches++;
        pending_ = std::move(done);
    }
    void cancel() override { pending_ = nullptr; }

    bool inFlight() const { return static_cast<bool>(pending_); }

    void answerWith(const UpdateManifest& manifest) {
        auto done = std::exchange(pending_, nullptr);
        if (done) { done(ManifestFetchResult::ok(manifest, QByteArray())); }
    }
    void failWith(UpdateError error) {
        auto done = std::exchange(pending_, nullptr);
        if (done) { done(ManifestFetchResult::failed(error)); }
    }

    int fetches = 0;

  private:
    Callback pending_;
};

UpdateManifest manifest(const QString& version, const QString& minimum = QStringLiteral("0.1.0")) {
    UpdateManifest m;
    m.schema = 1;
    m.product = QStringLiteral("dish-linux");
    m.version = version;
    m.channel = QStringLiteral("stable");
    m.minimumSupportedVersion = minimum;
    m.releaseNotesUrl =
        QStringLiteral("https://github.com/TinkerNorth/dish-linux/releases/tag/") + version;
    return m;
}

// QSettings with no injected path reaches the user's real config, which the
// checker uses for its last-check bookkeeping. Point the whole process at a
// temp tree for the duration of a test.
class ScopedSettingsPath {
  public:
    ScopedSettingsPath() {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }
    ~ScopedSettingsPath() { QSettings().clear(); }

    ScopedSettingsPath(const ScopedSettingsPath&) = delete;
    ScopedSettingsPath& operator=(const ScopedSettingsPath&) = delete;

  private:
    QTemporaryDir dir_;
};

// Tag for the relaunch case: reuse the settings path already in scope, so the
// second checker reads the first one's last-check timestamp. A Harness that
// made its own would be a different machine, not a relaunch.
struct ShareAmbientSettings {};

struct Harness {
    std::optional<ScopedSettingsPath> settingsPath;
    QTemporaryDir prefsDir;
    std::unique_ptr<UpdatePreferenceStore> prefs;
    FakeManifestGateway* gateway = nullptr;
    std::unique_ptr<UpdateChecker> checker;

    explicit Harness(qint64 nowMs = 1'000'000'000) {
        settingsPath.emplace();
        build(nowMs);
    }

    Harness(ShareAmbientSettings, qint64 nowMs) { build(nowMs); }

  private:
    void build(qint64 nowMs) {
        prefs = std::make_unique<UpdatePreferenceStore>(std::make_unique<QSettings>(
            prefsDir.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat));
        auto fake = std::make_unique<FakeManifestGateway>();
        gateway = fake.get();
        checker = std::make_unique<UpdateChecker>(prefs.get(), std::move(fake));
        checker->setClock([nowMs] { return nowMs; });
        checker->setCurrentVersion(QStringLiteral("0.1.0"));
    }
};

} // namespace

TEST_CASE("checker: start arms the startup delay and fetches nothing yet", "[update][checker]") {
    Harness h;
    h.checker->start();
    CHECK(h.checker->pendingCheckDelayMs() == kStartupDelayMs);
    CHECK(h.gateway->fetches == 0);
    CHECK(h.checker->snapshot().phase == UpdatePhase::Idle);
}

TEST_CASE("checker: a fired check fetches, and the manifest lands as Available",
          "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();

    CHECK(h.checker->snapshot().phase == UpdatePhase::Checking);
    CHECK(h.gateway->fetches == 1);
    CHECK(h.gateway->inFlight());

    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));
    const auto status = h.checker->snapshot();
    CHECK(status.phase == UpdatePhase::Available);
    CHECK(status.availableVersion == QStringLiteral("0.2.0"));
    // The periodic cadence is re-armed off the same manifest.
    CHECK(h.checker->pendingCheckDelayMs() == kPeriodicIntervalMs);
}

TEST_CASE("checker: a completed check records the time it completed", "[update][checker]") {
    constexpr qint64 kNow = 1'700'000'000'000;
    Harness h(kNow);
    CHECK_FALSE(h.checker->lastCheck().isValid());

    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));

    REQUIRE(h.checker->lastCheck().isValid());
    CHECK(h.checker->lastCheck().toMSecsSinceEpoch() == kNow);
}

TEST_CASE("checker: a start inside the minimum gap defers instead of re-checking",
          "[update][checker]") {
    constexpr qint64 kNow = 1'700'000'000'000;
    Harness h(kNow);
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));

    // A relaunch five minutes later, against the SAME stored timestamp, must
    // not fetch again on the startup delay.
    Harness relaunch{ShareAmbientSettings{}, kNow + 5 * 60 * 1000};
    relaunch.checker->start();
    CHECK(relaunch.gateway->fetches == 0);
    CHECK(relaunch.checker->pendingCheckDelayMs() > kStartupDelayMs);
}

TEST_CASE("checker: a failed fetch settles Failed and re-arms on the backoff",
          "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->failWith(UpdateError::Http);

    const auto status = h.checker->snapshot();
    CHECK(status.phase == UpdatePhase::Failed);
    CHECK(status.error == UpdateError::Http);
    CHECK(status.consecutiveFailures == 1);
    CHECK(h.checker->pendingCheckDelayMs() > 0);
}

TEST_CASE("checker: the manual check is rate-limited", "[update][checker]") {
    Harness h;
    h.checker->checkNow();
    CHECK(h.gateway->fetches == 1);
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));

    // Same injected clock, so no time has passed: a held-down button cannot
    // hammer the permalink.
    h.checker->checkNow();
    CHECK(h.gateway->fetches == 1);
}

TEST_CASE("checker: turning checks off disables it and arms no timer", "[update][checker]") {
    Harness h;
    h.checker->start();
    h.prefs->setChecksEnabled(false);

    CHECK(h.checker->snapshot().phase == UpdatePhase::Disabled);
    h.checker->firePendingCheck();
    CHECK(h.gateway->fetches == 0);
}

TEST_CASE("checker: skipping the offered version persists the skip", "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));
    REQUIRE(h.checker->snapshot().phase == UpdatePhase::Available);

    h.checker->skipAvailableVersion();
    CHECK(h.prefs->skippedVersion() == QStringLiteral("0.2.0"));
    CHECK(h.checker->snapshot().phase == UpdatePhase::UpToDate);
    CHECK(h.checker->snapshot().availableVersion.isEmpty());
}

TEST_CASE("checker: skipping with nothing offered is a no-op", "[update][checker]") {
    Harness h;
    h.checker->skipAvailableVersion();
    CHECK(h.prefs->skippedVersion().isEmpty());
}

TEST_CASE("checker: a required update surfaces as required", "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0"), QStringLiteral("0.2.0")));

    const auto status = h.checker->snapshot();
    CHECK(status.phase == UpdatePhase::Available);
    CHECK(status.required);

    // A required update cannot be muted.
    h.checker->skipAvailableVersion();
    CHECK(h.checker->snapshot().phase == UpdatePhase::Available);
}

TEST_CASE("checker: a second start is ignored", "[update][checker]") {
    Harness h;
    h.checker->start();
    const int armed = h.checker->pendingCheckDelayMs();
    h.checker->start();
    CHECK(h.checker->pendingCheckDelayMs() == armed);
    CHECK(h.gateway->fetches == 0);
}
