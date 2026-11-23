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
#include "core/controllerdb/DirectModel.h"
#include "core/controllerdb/IssueMatch.h"
#include "core/controllerdb/PadModel.h"
#include "source/controllerdb/ControllerDbGateway.h"
#include "source/controllerdb/ControllerDbJson.h"
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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace dish::controllerdb;
using dish::composer::ControllerDbCoordinator;
using dish::input::usbparse::HidParser;
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
        QJsonDocument::fromJson(bundled.files.value(QStringLiteral("controllers.json")));
    QJsonObject root = records.object();
    root[QStringLiteral("dbVersion")] = dbVersion;
    out.files[QStringLiteral("controllers.json")] =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    QJsonObject files;
    for (const auto& [path, bytes] : out.files.asKeyValueRange()) {
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
        for (const auto& [path, bytes] : files.files.asKeyValueRange()) {
            port.replies[path] = ok(bytes);
        }
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

    CHECK(rig.port.requested.size() == 1 + static_cast<std::size_t>(v2.files.size()));
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
        bundled.files.value(QStringLiteral("controllers.json"));
    QJsonObject manifest = QJsonDocument::fromJson(lying.manifest).object();
    QJsonObject files = manifest[QStringLiteral("files")].toObject();
    QJsonObject entry;
    const QByteArray records = lying.files.value(QStringLiteral("controllers.json"));
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

// The future-reader set (tests/fixtures/controllerdb-future, byte for byte): a dbVersion 2 the
// website may publish later, which a reader released today has to take. Every row of the set's
// EXPECT.md is one case below.
namespace {

QByteArray futureFile(const QString& path) {
    QFile file(QStringLiteral(DISH_FUTURE_DB_DIR) + QLatin1Char('/') + path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

// The whole set as the website serves it: the manifest and every file it names, strings/it.json
// (a locale this build does not ship) included.
DbFiles futureSet() {
    DbFiles out;
    out.manifest = futureFile(QStringLiteral("manifest.json"));
    const QJsonObject files =
        QJsonDocument::fromJson(out.manifest).object().value(QStringLiteral("files")).toObject();
    for (const QString& path : files.keys()) { out.files.insert(path, futureFile(path)); }
    return out;
}

// The rig after one check against the future set.
struct FutureRig : CheckRig {
    FutureRig() {
        serve(futureSet());
        coordinator.checkNow();
    }
    const ControllerCatalog& active() const { return *store.active(); }
    const ControllerCatalog& bundledCatalog() const { return *store.bundled(); }
    PadModel modelFor(int vendorId, int productId) const {
        return padModelFor(vendorId, productId, store.active().get(), store.bundled().get());
    }
    PadModel bundledModelFor(int vendorId, int productId) const {
        return padModelFor(vendorId, productId, store.bundled().get(), store.bundled().get());
    }
};

constexpr std::array<const char*, 9> kFutureRecordIds{
    "sony-dualsense",        "xbox-360-controller",    "future-pad-served",
    "future-pad-served-ds4", "future-pad-new-family",  "future-pad-new-op",
    "8bitdo-m30-d-input",    "future-pad-misauthored", "valve-steam-controller-bluetooth"};

} // namespace

TEST_CASE("future reader: the set is taken whole and is the database in force",
          "[controllerdb][future]") {
    FutureRig rig;
    CHECK(rig.lastResult() == std::optional<CheckResult>(CheckResult::Updated));
    CHECK(rig.store.state().value().activeVersion == 2);
    CHECK(rig.store.state().value().source == DbSource::Cache);
    // Replaced whole: a dbVersion 1 record the set does not carry is gone, nothing merged.
    REQUIRE(rig.bundledCatalog().findById("sony-dualshock-4-cuh-zct1") != nullptr);
    CHECK(rig.active().findById("sony-dualshock-4-cuh-zct1") == nullptr);
    SECTION("a fresh start reads the stored set back in force") {
        ControllerDbStore again(&rig.prefs, QStringLiteral(DISH_CONTROLLER_DB_DIR),
                                rig.dir.filePath(QStringLiteral("cache")));
        again.load();
        CHECK(again.state().value().activeVersion == 2);
    }
}

TEST_CASE("future reader: the manifest's unknown signature key is ignored",
          "[controllerdb][future]") {
    const auto parsed =
        dish::source::controllerdb::parseDbManifest(futureFile(QStringLiteral("manifest.json")));
    const auto* manifest = std::get_if<DbManifest>(&parsed);
    REQUIRE(manifest != nullptr);
    CHECK(manifest->dbVersion == 2);
    CHECK(manifestIsUsable(*manifest));
}

TEST_CASE("future reader: a strings file for a locale the build does not ship is left out",
          "[controllerdb][future]") {
    FutureRig rig;
    REQUIRE(rig.lastResult() == std::optional<CheckResult>(CheckResult::Updated));
    const bool askedForIt =
        std::find(rig.port.requested.begin(), rig.port.requested.end(),
                  QStringLiteral("strings/it.json")) != rig.port.requested.end();
    CHECK_FALSE(askedForIt);
    const QDir stored(rig.dir.filePath(QStringLiteral("cache/2")));
    CHECK(stored.entryList(QDir::Files) ==
          QStringList{QStringLiteral("controllers.json"), QStringLiteral("manifest.json")});
    CHECK(QDir(stored.filePath(QStringLiteral("strings"))).entryList(QDir::Files) ==
          QStringList{QStringLiteral("bs.json"), QStringLiteral("de.json"),
                      QStringLiteral("en.json"), QStringLiteral("es.json"),
                      QStringLiteral("fr.json"), QStringLiteral("pt-BR.json")});
}

TEST_CASE("future reader: a strings path that is not a locale still refuses the manifest",
          "[controllerdb][future]") {
    DbManifest m = manifestOf(2);
    for (const char* path : {"strings/../../dish.json", "strings/it/x.json", "strings/.json",
                             "strings/it.txt", "schema/it.json", "strings/it.json/"}) {
        INFO(path);
        DbManifest bad = m;
        bad.files.push_back(ManifestFile{path, kSomeSha, 10});
        CHECK_FALSE(manifestIsUsable(bad));
    }
    m.files.push_back(ManifestFile{"strings/it.json", kSomeSha, 10});
    m.files.push_back(ManifestFile{"strings/zh-Hant-TW.json", kSomeSha, 10});
    CHECK(manifestIsUsable(m));
    const std::vector<ManifestFile> fetched = filesThisBuildFetches(m);
    CHECK(fetched == std::vector<ManifestFile>{m.files[0], m.files[1]});
}

TEST_CASE("future reader: the records file's unknown generatedBy key is ignored",
          "[controllerdb][future]") {
    const auto parsed = dish::source::controllerdb::parseControllerDb(
        futureFile(QStringLiteral("controllers.json")));
    const auto* db = std::get_if<ControllerDb>(&parsed);
    REQUIRE(db != nullptr);
    CHECK(db->dbVersion == 2);
}

TEST_CASE("future reader: no record's trouble removes another", "[controllerdb][future]") {
    FutureRig rig;
    CHECK(rig.active().db().droppedRows == 0);
    CHECK(rig.active().db().records.size() == kFutureRecordIds.size());
    for (const char* id : kFutureRecordIds) {
        INFO(id);
        CHECK(rig.active().findById(id) != nullptr);
    }
}

TEST_CASE("future reader: the DualSense keeps every use it had, both issues and text os values",
          "[controllerdb][future]") {
    FutureRig rig;
    const ControllerRecord* served = rig.active().findUsb(DeviceId{0x054C, 0x0CE6});
    const ControllerRecord* before = rig.bundledCatalog().findUsb(DeviceId{0x054C, 0x0CE6});
    REQUIRE(served != nullptr);
    REQUIRE(before != nullptr);
    CHECK_FALSE(isInert(*served));
    CHECK(served->family == before->family);
    CHECK(served->input == before->input);
    CHECK(served->output == before->output);
    REQUIRE(served->issues.size() == 2U);
    CHECK(served->issues[0].id == "thor-ds5-rumble-usb");
    CHECK(served->issues[1].id == "future-os-as-text");
    // "linux": "6.8" matches by its major version; the windows, macos and futureos keys do not.
    const Issue& asText = served->issues[1];
    CHECK(issueApplies(asText, Transport::Usb, HostFacts{"Future Handheld", 6}));
    CHECK_FALSE(issueApplies(asText, Transport::Usb, HostFacts{"Future Handheld", 11}));
    CHECK_FALSE(issueApplies(asText, Transport::Usb, HostFacts{"Future Handheld", 1}));
}

TEST_CASE("future reader: a served row for a family the build lacks leaves the build's row",
          "[controllerdb][future]") {
    FutureRig rig;
    const ControllerRecord* served = rig.active().findUsb(DeviceId{0x045E, 0x028E});
    REQUIRE(served != nullptr);
    CHECK(isInert(*served));
    CHECK(rig.modelFor(0x045E, 0x028E) == rig.bundledModelFor(0x045E, 0x028E));
}

TEST_CASE("future reader: two new pads, one on a family the Direct path runs here",
          "[controllerdb][future]") {
    FutureRig rig;
    SECTION("xinput360 is xpad's on Linux, so 1209:0f02 is inert, named, never applied") {
        const ControllerRecord* served = rig.active().findUsb(DeviceId{0x1209, 0x0F02});
        REQUIRE(served != nullptr);
        CHECK(isInert(*served));
        CHECK_FALSE(rig.active().text("controller.future-pad-served.name", "en").empty());
        CHECK(rig.modelFor(0x1209, 0x0F02).source == ModelSource::Fallback);
    }
    SECTION("dualshock4 runs here, so 1209:0f05 resolves to the record at its next attach") {
        const ControllerRecord* served = rig.active().findUsb(DeviceId{0x1209, 0x0F05});
        REQUIRE(served != nullptr);
        CHECK_FALSE(isInert(*served));
        // No platforms.linux: the record's own auto, never Android's optIn or the Mac's rule.
        CHECK(served->directRule == DirectRule::Auto);
        const PadModel model = rig.modelFor(0x1209, 0x0F05);
        CHECK(model.source == ModelSource::Database);
        CHECK(model.parser == HidParser::DualShock4);
        CHECK(model.autoClaims);
    }
}

TEST_CASE("future reader: a new family or a new op is inert and never applied",
          "[controllerdb][future]") {
    FutureRig rig;
    for (const DeviceId id : {DeviceId{0x1209, 0x0F01}, DeviceId{0x1209, 0x0F03}}) {
        INFO(id.productId);
        const ControllerRecord* served = rig.active().findUsb(id);
        REQUIRE(served != nullptr);
        CHECK(isInert(*served));
        CHECK(rig.modelFor(id.vendorId, id.productId).source == ModelSource::Fallback);
    }
    CHECK_FALSE(rig.active().text("controller.future-pad-new-family.name", "en").empty());
}

TEST_CASE("future reader: a mapping in its reserved object form leaves the build's row",
          "[controllerdb][future]") {
    FutureRig rig;
    const ControllerRecord* served = rig.active().findUsb(DeviceId{0x2DC8, 0x5006});
    REQUIRE(served != nullptr);
    CHECK(isInert(*served));
    CHECK(rig.modelFor(0x2DC8, 0x5006) == rig.bundledModelFor(0x2DC8, 0x5006));
}

TEST_CASE("future reader: a misauthored record is inert and its issue never shows",
          "[controllerdb][future]") {
    FutureRig rig;
    const ControllerRecord* served = rig.active().findUsb(DeviceId{0x1209, 0x0F04});
    REQUIRE(served != nullptr);
    CHECK(isInert(*served));
    CHECK(applicableIssues(*served, Transport::Usb, HostFacts{"", 6}).empty());
    CHECK(rig.modelFor(0x1209, 0x0F04).source == ModelSource::Fallback);
}

TEST_CASE("future reader: the Bluetooth Steam Controller reads as in dbVersion 1",
          "[controllerdb][future]") {
    FutureRig rig;
    const ControllerRecord* served = rig.active().findBluetooth(DeviceId{0x28DE, 0x1106});
    const ControllerRecord* before = rig.bundledCatalog().findBluetooth(DeviceId{0x28DE, 0x1106});
    REQUIRE(served != nullptr);
    REQUIRE(before != nullptr);
    CHECK(*served == *before);
}
