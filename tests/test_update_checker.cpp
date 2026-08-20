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

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

using dish::reducer::UpdateError;
using dish::reducer::UpdateNotice;
using dish::reducer::UpdatePhase;
using dish::reducer::update_schedule::backoffDelayMs;
using dish::reducer::update_schedule::jitteredDelayMs;
using dish::reducer::update_schedule::kFutureSkewEscapeMs;
using dish::reducer::update_schedule::kMinCheckGapMs;
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
    ScopedSettingsPath() : savedFormat_(QSettings::defaultFormat()), savedPath_(userIniBasePath()) {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

    // Both settings are process-wide and outlive the temp dir. Leaving them set
    // points every later default-constructed QSettings — in this file and in
    // every test that runs after it — at a directory that no longer exists.
    ~ScopedSettingsPath() {
        QSettings().clear();
        QSettings::setDefaultFormat(savedFormat_);
        if (!savedPath_.isEmpty()) {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, savedPath_);
        }
    }

    ScopedSettingsPath(const ScopedSettingsPath&) = delete;
    ScopedSettingsPath& operator=(const ScopedSettingsPath&) = delete;

  private:
    // QSettings vends no path getter, so read the location back off a probe:
    // fileName() is "<base>/<organization>/<application>.ini".
    static QString userIniBasePath() {
        const QSettings probe(QSettings::IniFormat, QSettings::UserScope,
                              QCoreApplication::organizationName(),
                              QCoreApplication::applicationName());
        const QString file = probe.fileName();
        if (file.isEmpty()) { return {}; }
        QDir base = QFileInfo(file).dir();
        return base.cdUp() ? base.path() : QString();
    }

    QSettings::Format savedFormat_;
    QString savedPath_;
    QTemporaryDir dir_;
};

// Tag for the relaunch case: reuse the settings path already in scope, so the
// second checker reads the first one's last-check timestamp. A Harness that
// made its own would be a different machine, not a relaunch.
struct ShareAmbientSettings {};

// Tag for the arm where construction handed the checker no gateway at all.
struct NoGateway {};

struct Harness {
    std::optional<ScopedSettingsPath> settingsPath;
    QTemporaryDir prefsDir;
    std::unique_ptr<UpdatePreferenceStore> prefs;
    FakeManifestGateway* gateway = nullptr;
    // Every `notice` in emission order: the signal is the engine's only outward
    // announcement, and the facade turns each enum into a token.
    std::vector<std::pair<UpdateNotice, QString>> notices;
    std::unique_ptr<UpdateChecker> checker;

    explicit Harness(qint64 nowMs = 1'000'000'000) {
        settingsPath.emplace();
        build(nowMs, std::make_unique<FakeManifestGateway>());
    }

    Harness(ShareAmbientSettings, qint64 nowMs) {
        build(nowMs, std::make_unique<FakeManifestGateway>());
    }

    explicit Harness(NoGateway) {
        settingsPath.emplace();
        build(1'000'000'000, nullptr);
    }

  private:
    void build(qint64 nowMs, std::unique_ptr<FakeManifestGateway> fake) {
        prefs = std::make_unique<UpdatePreferenceStore>(std::make_unique<QSettings>(
            prefsDir.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat));
        gateway = fake.get();
        checker = std::make_unique<UpdateChecker>(prefs.get(), std::move(fake));
        QObject::connect(checker.get(), &UpdateChecker::notice,
                         [this](UpdateNotice kind, const QString& version) {
                             notices.emplace_back(kind, version);
                         });
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

TEST_CASE("checker: an available release is announced once, with its version",
          "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));

    REQUIRE(h.notices.size() == 1);
    CHECK(h.notices.front().first == UpdateNotice::Available);
    CHECK(h.notices.front().second == QStringLiteral("0.2.0"));
}

TEST_CASE("checker: an unsupported build is told the reason before the remedy",
          "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0"), QStringLiteral("0.2.0")));

    REQUIRE(h.notices.size() == 2);
    CHECK(h.notices[0].first == UpdateNotice::Unsupported);
    CHECK(h.notices[0].second == QStringLiteral("0.2.0"));
    CHECK(h.notices[1].first == UpdateNotice::Available);
    CHECK(h.notices[1].second == QStringLiteral("0.2.0"));
}

TEST_CASE("checker: an up-to-date check announces nothing", "[update][checker]") {
    Harness h;
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.1.0")));

    CHECK(h.checker->snapshot().phase == UpdatePhase::UpToDate);
    CHECK(h.notices.empty());
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

TEST_CASE("checker: a clock that moved backwards is trusted inside the escape window",
          "[update][checker]") {
    constexpr qint64 kNow = 1'700'000'000'000;
    Harness h(kNow);
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));

    // The stored timestamp is now in the FUTURE. Inside the escape window that
    // is a clock nudge, not a reason to re-check: defer, and never longer than
    // one gap.
    Harness nudged{ShareAmbientSettings{}, kNow - kFutureSkewEscapeMs / 2};
    nudged.checker->start();
    CHECK(nudged.gateway->fetches == 0);
    CHECK(nudged.checker->pendingCheckDelayMs() == static_cast<int>(kMinCheckGapMs));
}

TEST_CASE("checker: a clock beyond the escape window is ignored, not trusted",
          "[update][checker]") {
    constexpr qint64 kNow = 1'700'000'000'000;
    Harness h(kNow);
    h.checker->start();
    h.checker->firePendingCheck();
    h.gateway->answerWith(manifest(QStringLiteral("0.2.0")));

    // Far enough out that the stored time cannot be a nudge. One extra check
    // beats permanent silence.
    Harness jumped{ShareAmbientSettings{}, kNow - 2 * kFutureSkewEscapeMs};
    jumped.checker->start();
    CHECK(jumped.checker->pendingCheckDelayMs() == kStartupDelayMs);
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

TEST_CASE("checker: only a failed check is jittered", "[update][checker]") {
    SECTION("a failure lands inside the band around its backoff rung") {
        Harness h;
        h.checker->start();
        h.checker->firePendingCheck();
        h.gateway->failWith(UpdateError::Http);

        // The draw is a real random one, so the band — not a value — is the
        // assertable part; the edges come from the ladder itself.
        const int base = backoffDelayMs(1);
        const int armed = h.checker->pendingCheckDelayMs();
        CHECK(armed >= jitteredDelayMs(base, 0.0));
        CHECK(armed <= jitteredDelayMs(base, 1.0));
        // And the band is a band: an unjittered ladder would put a whole fleet
        // back on the permalink at the same instant.
        CHECK(jitteredDelayMs(base, 0.0) < base);
        CHECK(jitteredDelayMs(base, 1.0) > base);
    }
    SECTION("the periodic cadence is armed verbatim") {
        Harness h;
        h.checker->start();
        h.checker->firePendingCheck();
        h.gateway->answerWith(manifest(QStringLiteral("0.1.0")));
        REQUIRE(h.checker->snapshot().phase == UpdatePhase::UpToDate);
        CHECK(h.checker->pendingCheckDelayMs() == kPeriodicIntervalMs);
    }
}

TEST_CASE("checker: a check with no gateway settles as a transport failure", "[update][checker]") {
    // The default constructor always builds one, but the injecting constructor
    // can be handed nothing; the check must fail rather than hang in Checking
    // with no callback ever coming.
    Harness h{NoGateway{}};
    REQUIRE(h.gateway == nullptr);

    h.checker->checkNow();

    const auto status = h.checker->snapshot();
    CHECK(status.phase == UpdatePhase::Failed);
    CHECK(status.error == UpdateError::Http);
    CHECK(status.consecutiveFailures == 1);
}

TEST_CASE("checker: firing with nothing armed is a no-op", "[update][checker]") {
    Harness h;
    // start() has not run, so no delay is pending and the seam must not
    // manufacture a check out of one.
    REQUIRE(h.checker->pendingCheckDelayMs() == -1);
    h.checker->firePendingCheck();
    CHECK(h.gateway->fetches == 0);
    CHECK(h.checker->snapshot().phase == UpdatePhase::Idle);

    // An armed delay is consumed exactly once.
    h.checker->start();
    REQUIRE(h.checker->pendingCheckDelayMs() == kStartupDelayMs);
    h.checker->firePendingCheck();
    CHECK(h.gateway->fetches == 1);
    CHECK(h.checker->pendingCheckDelayMs() == -1);
    h.checker->firePendingCheck();
    CHECK(h.gateway->fetches == 1);
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

    // A required update cannot be muted — and the skip must not reach the store
    // either: a persisted skippedVersion comes back as PrefsChanged and would
    // mute the unsupported-build nag for good.
    h.checker->skipAvailableVersion();
    CHECK(h.checker->snapshot().phase == UpdatePhase::Available);
    CHECK(h.checker->snapshot().required);
    CHECK(h.checker->snapshot().availableVersion == QStringLiteral("0.2.0"));
    CHECK(h.prefs->skippedVersion().isEmpty());
}

TEST_CASE("checker: a second start is ignored", "[update][checker]") {
    Harness h;
    h.checker->start();
    const int armed = h.checker->pendingCheckDelayMs();
    h.checker->start();
    CHECK(h.checker->pendingCheckDelayMs() == armed);
    CHECK(h.gateway->fetches == 0);
}
