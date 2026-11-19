// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database check (docs/controller-db.md section 6.3 and 6.4):
// the pure decisions, then the coordinator's whole flow against a fake port
// that counts requests and a store over a temporary cache folder. The bundled
// copy is the real one from assets/controllers/v1; a "newer" database is that
// copy re-published as dbVersion 2 with its own manifest.

#include "composer/ControllerDbCoordinator.h"
#include "core/controllerdb/DbFetch.h"
#include "source/controllerdb/ControllerDbGateway.h"
#include "source/controllerdb/ControllerDbStore.h"
#include "source/store/ControllerDbPreferenceStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dish::controllerdb;
using dish::composer::ControllerDbCoordinator;
using dish::source::ControllerDbPreferenceStore;
using dish::source::controllerdb::ControllerDbPort;
using dish::source::controllerdb::ControllerDbStore;
using dish::source::controllerdb::DbFiles;
using dish::source::controllerdb::DbReply;
using dish::source::controllerdb::readDbFolder;

namespace {

const std::string kSomeSha(64, 'a');

DbManifest manifestOf(int dbVersion) {
    DbManifest m;
    m.dbVersion = dbVersion;
    m.files = {ManifestFile{"controllers.json", kSomeSha, 100},
               ManifestFile{"strings/en.json", kSomeSha, 10}};
    return m;
}

// A server: one reply per path, every request counted, none answered once `silent`.
class FakePort final : public ControllerDbPort {
  public:
    std::map<QString, DbReply> replies;
    std::vector<QString> requested;
    std::vector<QByteArray> ifNoneMatch;
    bool silent = false;
    Callback pending;
    int cancels = 0;

    void get(const QString& path, const QByteArray& etag, Callback done) override {
        requested.push_back(path);
        ifNoneMatch.push_back(etag);
        if (silent) {
            pending = std::move(done);
            return;
        }
        const auto it = replies.find(path);
        done(it != replies.end() ? it->second : DbReply{true, 0, {}, {}});
    }
    void cancel() override {
        ++cancels;
        pending = {};
    }
};

DbReply ok(const QByteArray& body, const QByteArray& etag = {}) { return {false, 200, body, etag}; }

// The bundled files re-published as `dbVersion`, every file and the manifest rewritten to match.
DbFiles republished(const DbFiles& bundled, int dbVersion) {
    DbFiles out = bundled;
    QJsonDocument records =
        QJsonDocument::fromJson(bundled.files.at(QStringLiteral("controllers.json")));
    QJsonObject root = records.object();
    root[QStringLiteral("dbVersion")] = dbVersion;
    out.files[QStringLiteral("controllers.json")] =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    QJsonObject files;
    for (const auto& [path, bytes] : out.files) {
        QJsonObject entry;
        entry[QStringLiteral("sha256")] = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        entry[QStringLiteral("bytes")] = static_cast<qint64>(bytes.size());
        files[path] = entry;
    }
    QJsonObject manifest;
    manifest[QStringLiteral("schema")] = 1;
    manifest[QStringLiteral("dbVersion")] = dbVersion;
    manifest[QStringLiteral("publishedAt")] = QStringLiteral("2026-11-20T00:00:00Z");
    manifest[QStringLiteral("files")] = files;
    out.manifest = QJsonDocument(manifest).toJson();
    return out;
}

// The coordinator over a fresh cache folder and preferences, the bundled copy the real one.
struct CheckRig {
    QTemporaryDir dir;
    ControllerDbPreferenceStore prefs{std::make_unique<QSettings>(
        dir.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat)};
    ControllerDbStore store{&prefs, QStringLiteral(DISH_CONTROLLER_DB_DIR),
                            dir.filePath(QStringLiteral("cache"))};
    FakePort port;
    std::int64_t now = 1'763'586'000'000;
    ControllerDbCoordinator coordinator{&store, &prefs, &port, [this] { return now; }};

    CheckRig() { store.load(); }

    void serve(const DbFiles& files, const QByteArray& etag = "\"v2\"") {
        port.replies[QStringLiteral("manifest.json")] = ok(files.manifest, etag);
        for (const auto& [path, bytes] : files.files) { port.replies[path] = ok(bytes); }
    }
    std::optional<CheckResult> lastResult() const { return store.state().value().lastCheckResult; }
};

} // namespace

TEST_CASE("controller db fetch: the switch is the whole rule, Check now included",
          "[controllerdb][fetch]") {
    CHECK(checkMayRun(true, CheckTrigger::Schedule));
    CHECK(checkMayRun(true, CheckTrigger::CheckNow));
    CHECK_FALSE(checkMayRun(false, CheckTrigger::Schedule));
    CHECK_FALSE(checkMayRun(false, CheckTrigger::CheckNow));
}

TEST_CASE("controller db fetch: the schedule is ten seconds, then six hours",
          "[controllerdb][fetch]") {
    CHECK(kFirstCheckDelayMs == 10'000);
    CHECK(kCheckIntervalMs == 6LL * 3600 * 1000);
    CHECK(kDbBaseUrl == "https://dish.tinkernorth.com/controllers/v1/");
}

TEST_CASE("controller db fetch: the ETag is sent only for the database it came with",
          "[controllerdb][fetch]") {
    CHECK(ifNoneMatchFor("\"abc\"", 3, 3) == std::optional<std::string>("\"abc\""));
    CHECK_FALSE(ifNoneMatchFor("\"abc\"", 2, 3).has_value());
    CHECK_FALSE(ifNoneMatchFor("", 3, 3).has_value());
}

TEST_CASE("controller db fetch: a reply's transport and status, before its body",
          "[controllerdb][fetch]") {
    CHECK(replyVerdict(true, 0) == ReplyVerdict::Offline);
    CHECK(replyVerdict(false, 503) == ReplyVerdict::Offline);
    CHECK(replyVerdict(false, 304) == ReplyVerdict::NotModified);
    CHECK(replyVerdict(false, 200) == ReplyVerdict::Body);
    CHECK(replyVerdict(false, 404) == ReplyVerdict::Invalid);
    CHECK(replyVerdict(false, 413) == ReplyVerdict::Invalid);
}

TEST_CASE("controller db fetch: a usable manifest", "[controllerdb][fetch]") {
    CHECK(manifestIsUsable(manifestOf(2)));
    SECTION("schema 2 is a later client's") {
        DbManifest m = manifestOf(2);
        m.schema = 2;
        CHECK_FALSE(manifestIsUsable(m));
    }
    SECTION("no version") { CHECK_FALSE(manifestIsUsable(manifestOf(0))); }
    SECTION("no records") {
        DbManifest m = manifestOf(2);
        m.files.erase(m.files.begin());
        CHECK_FALSE(manifestIsUsable(m));
    }
    SECTION("no English strings") {
        DbManifest m = manifestOf(2);
        m.files.pop_back();
        CHECK_FALSE(manifestIsUsable(m));
    }
    SECTION("a path outside the database is never fetched or written") {
        DbManifest m = manifestOf(2);
        m.files.push_back(ManifestFile{"../../dish.exe", kSomeSha, 10});
        CHECK_FALSE(manifestIsUsable(m));
    }
    SECTION("a digest that is not 64 lowercase hex") {
        DbManifest m = manifestOf(2);
        m.files[0].sha256 = std::string(64, 'A');
        CHECK_FALSE(manifestIsUsable(m));
    }
    SECTION("an empty or oversize file") {
        DbManifest m = manifestOf(2);
        m.files[0].bytes = 0;
        CHECK_FALSE(manifestIsUsable(m));
        m.files[0].bytes = kFileMaxBytes + 1;
        CHECK_FALSE(manifestIsUsable(m));
    }
}

TEST_CASE("controller db fetch: only a later database is downloaded", "[controllerdb][fetch]") {
    CHECK(manifestStep(manifestOf(2), 1) == ManifestStep::Download);
    CHECK(manifestStep(manifestOf(1), 1) == ManifestStep::UpToDate);
    CHECK(manifestStep(manifestOf(1), 2) == ManifestStep::UpToDate);
}

TEST_CASE("controller db fetch: a file is the one promised by size and digest",
          "[controllerdb][fetch]") {
    const ManifestFile promised{"controllers.json", kSomeSha, 100};
    CHECK(fileMatches(promised, kSomeSha, 100));
    CHECK_FALSE(fileMatches(promised, kSomeSha, 99));
    CHECK_FALSE(fileMatches(promised, std::string(64, 'b'), 100));
}

TEST_CASE("controller db fetch: the records must be schema 1 and the announced version",
          "[controllerdb][fetch]") {
    ControllerDb db;
    db.dbVersion = 2;
    CHECK(recordsMatchManifest(db, manifestOf(2)));
    CHECK_FALSE(recordsMatchManifest(db, manifestOf(3)));
    db.schema = 2;
    CHECK_FALSE(recordsMatchManifest(db, manifestOf(2)));
}

TEST_CASE("controller db fetch: the result words", "[controllerdb][fetch]") {
    for (const CheckResult r : {CheckResult::UpToDate, CheckResult::Updated, CheckResult::Offline,
                                CheckResult::Invalid, CheckResult::Off}) {
        CHECK(checkResultFrom(checkResultToken(r)) == std::optional<CheckResult>(r));
    }
    CHECK(checkResultToken(CheckResult::UpToDate) == "upToDate");
    CHECK_FALSE(checkResultFrom("maybe").has_value());
}

TEST_CASE("controller db check: with the switch off nothing leaves the PC",
          "[controllerdb][check]") {
    CheckRig rig;
    rig.prefs.setChecksEnabled(false);
    rig.coordinator.scheduledCheck();
    rig.coordinator.checkNow();
    CHECK(rig.port.requested.empty());
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Off));
    CHECK(rig.store.state().value().activeVersion == 1);
}

TEST_CASE("controller db check: the switch defaults on", "[controllerdb][check]") {
    CheckRig rig;
    CHECK(rig.prefs.checksEnabled());
    CHECK(QString::fromLatin1(ControllerDbPreferenceStore::kKeyChecksEnabled) ==
          QStringLiteral("controller_db_check_enabled"));
}

TEST_CASE("controller db check: the bundled copy is in force before any check",
          "[controllerdb][check]") {
    CheckRig rig;
    const auto status = rig.store.state().value();
    CHECK(status.bundledVersion == 1);
    CHECK(status.activeVersion == 1);
    CHECK(status.source == DbSource::Bundled);
    CHECK_FALSE(status.lastCheckResult.has_value());
}

TEST_CASE("controller db check: the manifest for the version in force is up to date",
          "[controllerdb][check]") {
    CheckRig rig;
    rig.serve(readDbFolder(QStringLiteral(DISH_CONTROLLER_DB_DIR)), "\"v1\"");
    rig.coordinator.checkNow();
    CHECK(rig.port.requested == std::vector<QString>{QStringLiteral("manifest.json")});
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::UpToDate));
    CHECK(rig.store.state().value().lastCheckAtMs == rig.now);

    SECTION("the next check sends its ETag and takes a 304 as up to date") {
        rig.port.replies[QStringLiteral("manifest.json")] = DbReply{false, 304, {}, {}};
        rig.coordinator.scheduledCheck();
        CHECK(rig.port.ifNoneMatch.back() == QByteArray("\"v1\""));
        CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::UpToDate));
    }
}

TEST_CASE("controller db check: a newer database is fetched, verified and put in force",
          "[controllerdb][check]") {
    CheckRig rig;
    const DbFiles v2 = republished(readDbFolder(QStringLiteral(DISH_CONTROLLER_DB_DIR)), 2);
    rig.serve(v2);
    rig.coordinator.checkNow();

    CHECK(rig.port.requested.size() == 1 + v2.files.size());
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Updated));
    const auto status = rig.store.state().value();
    CHECK(status.activeVersion == 2);
    CHECK(status.bundledVersion == 1);
    CHECK(status.source == DbSource::Cache);
    CHECK(rig.prefs.etagVersion() == 2);

    SECTION("a fresh start reads the cache back in force") {
        ControllerDbStore again(&rig.prefs, QStringLiteral(DISH_CONTROLLER_DB_DIR),
                                rig.dir.filePath(QStringLiteral("cache")));
        again.load();
        CHECK(again.state().value().activeVersion == 2);
        CHECK(again.active()->db().records.size() == rig.store.bundled()->db().records.size());
    }
    SECTION("a cache tampered with on disk is ignored for the bundled copy") {
        QFile records(rig.dir.filePath(QStringLiteral("cache/2/controllers.json")));
        REQUIRE(records.open(QIODevice::Append));
        records.write(" ");
        records.close();
        ControllerDbStore again(&rig.prefs, QStringLiteral(DISH_CONTROLLER_DB_DIR),
                                rig.dir.filePath(QStringLiteral("cache")));
        again.load();
        CHECK(again.state().value().activeVersion == 1);
        CHECK(again.state().value().source == DbSource::Bundled);
    }
}

TEST_CASE("controller db check: a file that is not the one promised keeps the old database",
          "[controllerdb][check]") {
    CheckRig rig;
    const DbFiles v2 = republished(readDbFolder(QStringLiteral(DISH_CONTROLLER_DB_DIR)), 2);
    rig.serve(v2);
    rig.port.replies[QStringLiteral("strings/fr.json")] = ok(QByteArray("{}"));
    rig.coordinator.checkNow();
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Invalid));
    CHECK(rig.store.state().value().activeVersion == 1);
    CHECK_FALSE(QFile::exists(rig.dir.filePath(QStringLiteral("cache/current"))));
}

TEST_CASE("controller db check: records that disagree with the manifest's version are refused",
          "[controllerdb][check]") {
    CheckRig rig;
    const DbFiles bundled = readDbFolder(QStringLiteral(DISH_CONTROLLER_DB_DIR));
    // A manifest announcing 2 over records that still say 1, every digest honest.
    DbFiles lying = republished(bundled, 2);
    lying.files[QStringLiteral("controllers.json")] =
        bundled.files.at(QStringLiteral("controllers.json"));
    QJsonObject manifest = QJsonDocument::fromJson(lying.manifest).object();
    QJsonObject files = manifest[QStringLiteral("files")].toObject();
    QJsonObject entry;
    const QByteArray records = lying.files.at(QStringLiteral("controllers.json"));
    entry[QStringLiteral("sha256")] =
        QString::fromLatin1(QCryptographicHash::hash(records, QCryptographicHash::Sha256).toHex());
    entry[QStringLiteral("bytes")] = static_cast<qint64>(records.size());
    files[QStringLiteral("controllers.json")] = entry;
    manifest[QStringLiteral("files")] = files;
    lying.manifest = QJsonDocument(manifest).toJson();
    rig.serve(lying);
    rig.coordinator.checkNow();
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Invalid));
    CHECK(rig.store.state().value().activeVersion == 1);
}

TEST_CASE("controller db check: an unreachable host is offline", "[controllerdb][check]") {
    CheckRig rig;
    rig.coordinator.checkNow();
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Offline));
    SECTION("and a server error the same") {
        rig.port.replies[QStringLiteral("manifest.json")] = DbReply{false, 502, {}, {}};
        rig.coordinator.checkNow();
        CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Offline));
    }
}

TEST_CASE("controller db check: a portal page instead of a manifest is invalid",
          "[controllerdb][check]") {
    CheckRig rig;
    rig.port.replies[QStringLiteral("manifest.json")] = ok("<html>Sign in to Wi-Fi</html>");
    rig.coordinator.checkNow();
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Invalid));
}

TEST_CASE("controller db check: switching off mid-check drops it", "[controllerdb][check]") {
    CheckRig rig;
    rig.port.silent = true;
    rig.coordinator.checkNow();
    REQUIRE(rig.coordinator.inFlight());
    rig.prefs.setChecksEnabled(false);
    CHECK_FALSE(rig.coordinator.inFlight());
    CHECK(rig.port.cancels >= 1);
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Off));
}

TEST_CASE("controller db check: a check in flight is not started twice", "[controllerdb][check]") {
    CheckRig rig;
    rig.port.silent = true;
    rig.coordinator.checkNow();
    rig.coordinator.scheduledCheck();
    CHECK(rig.port.requested.size() == 1U);
}
