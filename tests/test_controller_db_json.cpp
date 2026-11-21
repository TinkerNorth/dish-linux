// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database's JSON reader: what refuses a whole file, what drops
// one row and keeps the rest, what keeps a row inert, and that the Linux
// platform override is applied as the row is read.

#include "core/controllerdb/DbFetch.h"
#include "core/controllerdb/DirectModel.h"
#include "source/controllerdb/ControllerDbJson.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>

#include <string>
#include <variant>

using namespace dish::controllerdb;
using dish::source::controllerdb::DbParseError;
using dish::source::controllerdb::parseControllerDb;
using dish::source::controllerdb::parseDbManifest;
using dish::source::controllerdb::parseStringsTable;

namespace {

// One whole record in the contract's shape, with `ref`, `extra` and `platforms` to vary.
std::string recordJson(const std::string& id, const std::string& ref,
                       const std::string& extraInput = "", const std::string& platforms = "") {
    return R"({"schema":1,"id":")" + id + R"(","requires":{"families":[")" + ref +
           R"("],"ops":[]},"identity":{"usb":["054c:0ce6"],"bluetooth":[]},)"
           R"("input":{"protocol":{"ref":")" +
           ref + R"(","streamsUnprompted":true},)" + extraInput +
           R"("direct":{"rule":"auto"},"capabilities":{"motion":true,"touchpad":true,)"
           R"("trackpads":0,"battery":true,"extendedButtons":false}},)"
           R"("output":{"route":"interruptOut","init":{"kind":"none"},"capabilities":)"
           R"({"rumble":true,"triggerRumble":false,"lightbar":true,"playerLeds":true,)"
           R"("micLed":true,"triggerEffects":true}},)"
           R"("expect":{"endpoints":{"in":true,"out":null}},)" +
           platforms +
           R"("support":{"verification":{"state":"verified","transport":"usb"},)"
           R"("issues":[{"id":"thor-ds5-rumble-usb","device":"AYN Thor","os":{"android":15},)"
           R"("transport":"usb","lane":"output.rumble","status":"open","since":"2026-11-11",)"
           R"("reports":1}],"sources":[]}})";
}

QByteArray dbJson(const std::string& records, int dbVersion = 1, int schema = 1) {
    return QByteArray::fromStdString(R"({"schema":)" + std::to_string(schema) + R"(,"dbVersion":)" +
                                     std::to_string(dbVersion) + R"(,"records":[)" + records +
                                     "]}");
}

} // namespace

TEST_CASE("controller db json: a whole record decodes field by field", "[controllerdb][json]") {
    const auto parsed = parseControllerDb(dbJson(recordJson("sony-dualsense", "dualsense")));
    const auto* db = std::get_if<ControllerDb>(&parsed);
    REQUIRE(db != nullptr);
    CHECK(db->dbVersion == 1);
    REQUIRE(db->records.size() == 1U);
    const ControllerRecord& r = db->records[0];
    CHECK(r.id == "sony-dualsense");
    CHECK(r.family == Family::DualSense);
    CHECK(r.usb == std::vector<DeviceId>{DeviceId{0x054C, 0x0CE6}});
    CHECK(r.streamsUnprompted);
    CHECK(r.directRule == DirectRule::Auto);
    CHECK(r.input.motion);
    CHECK(r.route == OutputRoute::InterruptOut);
    CHECK(r.output.triggerEffects);
    CHECK(r.expect.endpointIn == Expectation::Present);
    CHECK(r.expect.endpointOut == Expectation::Unknown);
    CHECK(r.verification == Verification::Verified);
    REQUIRE(r.issues.size() == 1U);
    CHECK(r.issues[0].device == std::optional<std::string>("AYN Thor"));
    CHECK(r.issues[0].os == std::vector<IssueOs>{IssueOs{"android", "15"}});
}

TEST_CASE("controller db json: platforms.linux replaces the record's direct rule",
          "[controllerdb][json]") {
    const std::string linuxRule = R"("platforms":{"linux":{"direct":{"rule":"optIn"}}},)";
    const std::string windows = R"("platforms":{"windows":{"direct":{"rule":"whenOsCannot"}}},)";
    const auto mine = parseControllerDb(dbJson(recordJson("a", "dualsense", "", linuxRule)));
    const auto theirs = parseControllerDb(dbJson(recordJson("a", "dualsense", "", windows)));
    CHECK(std::get<ControllerDb>(mine).records[0].directRule == DirectRule::OptIn);
    CHECK(std::get<ControllerDb>(theirs).records[0].directRule == DirectRule::Auto);
}

TEST_CASE("controller db json: a hid record carries its mapping", "[controllerdb][json]") {
    const auto parsed =
        parseControllerDb(dbJson(recordJson("pdp", "hid", R"("mapping":"switch",)")));
    CHECK(std::get<ControllerDb>(parsed).records[0].mapping == Mapping::Switch);
}

TEST_CASE("controller db json: a null route is a pad with no output", "[controllerdb][json]") {
    std::string record = recordJson("a", "dualsense");
    const std::string route = R"("route":"interruptOut")";
    record.replace(record.find(route), route.size(), R"("route":null)");
    const auto parsed = parseControllerDb(dbJson(record));
    CHECK(std::get<ControllerDb>(parsed).records[0].route == OutputRoute::None);
}

TEST_CASE("controller db json: a family a later database adds keeps the row, inert",
          "[controllerdb][json]") {
    const auto parsed = parseControllerDb(dbJson(recordJson("future", "joyConPair")));
    const auto& db = std::get<ControllerDb>(parsed);
    REQUIRE(db.records.size() == 1U);
    CHECK(db.records[0].family == Family::Unknown);
    CHECK(isInert(db.records[0]));
    CHECK(db.droppedRows == 0);
}

TEST_CASE("controller db json: a row that does not decode is dropped and counted",
          "[controllerdb][json]") {
    std::string broken = recordJson("broken", "dualsense");
    const std::string state = R"("state":"verified")";
    broken.replace(broken.find(state), state.size(), R"("state":"rumoured")");
    const auto parsed = parseControllerDb(
        dbJson(recordJson("good", "dualsense") + "," + broken + R"(,{"schema":2,"id":"x"})"));
    const auto& db = std::get<ControllerDb>(parsed);
    REQUIRE(db.records.size() == 1U);
    CHECK(db.records[0].id == "good");
    CHECK(db.droppedRows == 2);
}

TEST_CASE("controller db json: what refuses a whole file", "[controllerdb][json]") {
    CHECK(std::get<DbParseError>(parseControllerDb("<html>portal</html>")) ==
          DbParseError::BadJson);
    CHECK(std::get<DbParseError>(parseControllerDb("[]")) == DbParseError::BadJson);
    CHECK(std::get<DbParseError>(parseControllerDb(dbJson("", 1, 2))) ==
          DbParseError::UnsupportedSchema);
    CHECK(std::get<DbParseError>(parseControllerDb(dbJson("", 0))) == DbParseError::BadVersion);
    CHECK(std::get<DbParseError>(parseControllerDb(R"({"schema":1,"dbVersion":1})")) ==
          DbParseError::BadShape);
    const QByteArray huge(static_cast<qsizetype>(kFileMaxBytes + 1), ' ');
    CHECK(std::get<DbParseError>(parseControllerDb(huge)) == DbParseError::Oversize);
}

TEST_CASE("controller db json: a strings file is a flat table of text", "[controllerdb][json]") {
    const auto parsed = parseStringsTable(R"({"family.hid.name":"Generic HID gamepad"})");
    CHECK(std::get<StringsTable>(parsed).at("family.hid.name") == "Generic HID gamepad");
    CHECK(std::get<DbParseError>(parseStringsTable(R"({"a":1})")) == DbParseError::BadShape);
    CHECK(std::get<DbParseError>(parseStringsTable("nope")) == DbParseError::BadJson);
}

TEST_CASE("controller db json: the manifest lists each file's digest and size",
          "[controllerdb][json]") {
    const QByteArray body =
        R"({"schema":1,"dbVersion":3,"publishedAt":"2026-11-19T00:00:00Z","files":{)"
        R"("controllers.json":{"sha256":"aa","bytes":10},"strings/en.json":{"sha256":"bb","bytes":5}}})";
    const auto parsed = parseDbManifest(body);
    const auto& manifest = std::get<DbManifest>(parsed);
    CHECK(manifest.dbVersion == 3);
    CHECK(manifest.publishedAt == "2026-11-19T00:00:00Z");
    REQUIRE(manifest.files.size() == 2U);
    CHECK(manifest.files[0] == ManifestFile{"controllers.json", "aa", 10});
    CHECK(std::get<DbParseError>(parseDbManifest(R"({"schema":1,"dbVersion":3,"files":[]})")) ==
          DbParseError::BadShape);
    const QByteArray huge(static_cast<qsizetype>(kManifestMaxBytes + 1), ' ');
    CHECK(std::get<DbParseError>(parseDbManifest(huge)) == DbParseError::Oversize);
}
