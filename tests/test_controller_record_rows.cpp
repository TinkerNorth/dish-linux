// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database's rows on the Diagnostics page and in Settings, as
// tokens and words from the database's own strings (qml/DiagnosticsMaps.h):
// the record facts and the verification chip, the issues that apply here, the
// last write, the last test, the database's account of itself, and a flight
// recorder row as the bundle takes it.

#include "qml/DiagnosticsMaps.h"

#include <catch2/catch_test_macros.hpp>

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cerrno>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dish::controllerdb;
using dish::qml::RecordCardInputs;

namespace {

ControllerRecord dualSense() {
    ControllerRecord r;
    r.id = "sony-dualsense";
    r.family = Family::DualSense;
    r.requiresFamilies = {"dualsense"};
    r.usb = {DeviceId{0x054C, 0x0CE6}};
    r.verification = Verification::Verified;
    Issue thor;
    thor.id = "thor-ds5-rumble-usb";
    thor.device = "AYN Thor";
    thor.transport = Transport::Usb;
    thor.status = "open";
    Issue fixedOne = thor;
    fixedOne.id = "fixed-one";
    fixedOne.status = "fixed";
    r.issues = {thor, fixedOne};
    return r;
}

RecordCardInputs inputsFor(const ControllerRecord& record, const std::string& locale) {
    ControllerDb db;
    db.dbVersion = 1;
    db.records = {record};
    std::map<std::string, StringsTable> strings;
    strings["en"] = {{"controller.sony-dualsense.name", "DualSense Wireless Controller"},
                     {"family.dualsense.name", "DualSense protocol"},
                     {"issue.thor-ds5-rumble-usb.title", "Rumble is silent on the AYN Thor"},
                     {"issue.thor-ds5-rumble-usb.body", "Being looked into."}};
    strings["de"] = {{"family.dualsense.name", "DualSense-Protokoll"}};
    RecordCardInputs inputs;
    inputs.catalog = std::make_shared<const ControllerCatalog>(std::move(db), std::move(strings),
                                                               DbSource::Bundled);
    AttachedIdentity identity;
    identity.record = record;
    identity.mismatches = {"endpoints.out: expected true, found false"};
    inputs.identity = identity;
    inputs.locale = locale;
    inputs.host = HostFacts{"AYN Thor", 6};
    return inputs;
}

} // namespace

TEST_CASE("record row: a matched pad's record, words in the person's language",
          "[diagnostics][record-row]") {
    const QVariantMap row = dish::qml::controllerRecordRow(inputsFor(dualSense(), "de"));
    CHECK(row.value(QStringLiteral("matched")).toBool());
    CHECK(row.value(QStringLiteral("recordId")).toString() == QStringLiteral("sony-dualsense"));
    CHECK(row.value(QStringLiteral("name")).toString() ==
          QStringLiteral("DualSense Wireless Controller"));
    CHECK(row.value(QStringLiteral("family")).toString() == QStringLiteral("DualSense-Protokoll"));
    CHECK(row.value(QStringLiteral("chip")).toString() == QStringLiteral("verified"));
    CHECK_FALSE(row.value(QStringLiteral("inert")).toBool());
    CHECK(row.value(QStringLiteral("mismatches")).toStringList() ==
          QStringList{QStringLiteral("endpoints.out: expected true, found false")});
}

TEST_CASE("record row: the issue for this machine shows, with its title and body",
          "[diagnostics][record-row]") {
    const QVariantList issues = dish::qml::controllerRecordRow(inputsFor(dualSense(), "en"))
                                    .value(QStringLiteral("issues"))
                                    .toList();
    REQUIRE(issues.size() == 1);
    const QVariantMap issue = issues[0].toMap();
    CHECK(issue.value(QStringLiteral("title")).toString() ==
          QStringLiteral("Rumble is silent on the AYN Thor"));
    CHECK(issue.value(QStringLiteral("body")).toString() == QStringLiteral("Being looked into."));

    SECTION("and not on another machine, nor over Bluetooth") {
        RecordCardInputs elsewhere = inputsFor(dualSense(), "en");
        elsewhere.host.model = "Dell XPS 13";
        CHECK(dish::qml::controllerRecordRow(elsewhere)
                  .value(QStringLiteral("issues"))
                  .toList()
                  .isEmpty());
        RecordCardInputs wireless = inputsFor(dualSense(), "en");
        wireless.transport = Transport::Bluetooth;
        CHECK(dish::qml::controllerRecordRow(wireless)
                  .value(QStringLiteral("issues"))
                  .toList()
                  .isEmpty());
    }
}

TEST_CASE("record row: an imported record wears the layout-guessed chip",
          "[diagnostics][record-row]") {
    ControllerRecord imported = dualSense();
    imported.verification = Verification::Imported;
    CHECK(dish::qml::controllerRecordRow(inputsFor(imported, "en"))
              .value(QStringLiteral("chip"))
              .toString() == QStringLiteral("guessed"));
    imported.verification = Verification::Flagged;
    CHECK(dish::qml::controllerRecordRow(inputsFor(imported, "en"))
              .value(QStringLiteral("chip"))
              .toString() == QStringLiteral("flagged"));
}

TEST_CASE("record row: a pad no record lists, or no database at all, is unmatched",
          "[diagnostics][record-row]") {
    RecordCardInputs none = inputsFor(dualSense(), "en");
    none.identity = AttachedIdentity{};
    CHECK_FALSE(dish::qml::controllerRecordRow(none).value(QStringLiteral("matched")).toBool());
    RecordCardInputs noDb = inputsFor(dualSense(), "en");
    noDb.catalog = nullptr;
    CHECK_FALSE(dish::qml::controllerRecordRow(noDb).value(QStringLiteral("matched")).toBool());
}

TEST_CASE("record row: the last write and the last test, or nothing yet",
          "[diagnostics][record-row]") {
    CHECK(dish::qml::writeVerdictRow(std::nullopt).isEmpty());
    const QVariantMap write = dish::qml::writeVerdictRow(dish::diagnostics::WriteVerdict{
        true, dish::diagnostics::WriteRoute::SetReport, 0x80, false, ETIMEDOUT, 42});
    CHECK(write.value(QStringLiteral("route")).toString() == QStringLiteral("setReport"));
    CHECK(write.value(QStringLiteral("reportId")).toString() == QStringLiteral("80"));
    CHECK_FALSE(write.value(QStringLiteral("ok")).toBool());
    CHECK(write.value(QStringLiteral("err")).toString() == QStringLiteral("ETIMEDOUT"));
    CHECK(write.value(QStringLiteral("atMs")).toLongLong() == 42);

    CHECK(dish::qml::testOutcomeRow(std::nullopt).isEmpty());
    const QVariantMap test =
        dish::qml::testOutcomeRow(dish::diagnostics::TestOutcome{"rumble.both", "didNotWork", 7});
    CHECK(test.value(QStringLiteral("test")).toString() == QStringLiteral("rumble.both"));
    CHECK(test.value(QStringLiteral("result")).toString() == QStringLiteral("didNotWork"));
}

TEST_CASE("record row: the database's account of itself", "[diagnostics][record-row]") {
    dish::source::controllerdb::ControllerDbStatus status;
    status.bundledVersion = 1;
    status.activeVersion = 3;
    status.source = DbSource::Cache;
    QVariantMap row = dish::qml::controllerDbRow(status);
    CHECK(row.value(QStringLiteral("source")).toString() == QStringLiteral("cache"));
    CHECK(row.value(QStringLiteral("activeVersion")).toInt() == 3);
    CHECK(row.value(QStringLiteral("lastCheckResult")).toString().isEmpty());
    status.lastCheckResult = CheckResult::Offline;
    status.lastCheckAtMs = 99;
    row = dish::qml::controllerDbRow(status);
    CHECK(row.value(QStringLiteral("lastCheckResult")).toString() == QStringLiteral("offline"));
    CHECK(row.value(QStringLiteral("lastCheckAtMs")).toLongLong() == 99);
}

TEST_CASE("record row: a flight recorder row as the bundle's log takes it",
          "[diagnostics][record-row]") {
    QVariantMap link;
    link[QStringLiteral("atMs")] = qint64(5);
    link[QStringLiteral("kind")] = QStringLiteral("linkChanged");
    link[QStringLiteral("subject")] = QStringLiteral("Living Room PC");
    link[QStringLiteral("from")] = QStringLiteral("connecting");
    link[QStringLiteral("to")] = QStringLiteral("connected");
    const auto linkEvent = dish::qml::bundleLogEventOf(link);
    CHECK(linkEvent.subjectIsHost);
    CHECK(linkEvent.details == std::vector<std::string>{"connecting", "connected"});
    CHECK(linkEvent.host.empty());

    QVariantMap bound;
    bound[QStringLiteral("kind")] = QStringLiteral("padBound");
    bound[QStringLiteral("subject")] = QStringLiteral("Wireless Controller");
    bound[QStringLiteral("host")] = QStringLiteral("Living Room PC");
    const auto boundEvent = dish::qml::bundleLogEventOf(bound);
    CHECK_FALSE(boundEvent.subjectIsHost);
    CHECK(boundEvent.host == "Living Room PC");

    QVariantMap failed;
    failed[QStringLiteral("kind")] = QStringLiteral("padDirectFailed");
    failed[QStringLiteral("failure")] = QStringLiteral("busy");
    failed[QStringLiteral("host")] = QStringLiteral("stale label");
    const auto failedEvent = dish::qml::bundleLogEventOf(failed);
    CHECK(failedEvent.details == std::vector<std::string>{"busy"});
    CHECK(failedEvent.host.empty());
}
