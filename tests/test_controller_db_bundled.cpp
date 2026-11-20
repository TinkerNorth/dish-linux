// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database this client ships (assets/controllers/v1, the
// :/controllers/v1 resource), read through the same path the app reads it
// with. dbVersion 1 holds 272 records: dish-android's 102 kKnown rows and 172
// kImported rows, less the two kKnown ids listed twice (24c6:551a and
// 24c6:561a), which the database keeps only as their Xbox One records
// (docs/controller-db.md section 12). The compiled tables this client keeps
// are checked against the file here, so the two cannot drift apart.

#include "core/controllerdb/DirectModel.h"
#include "core/controllerdb/Vocabulary.h"
#include "core/input/UsbReportParsers.h"
#include "source/controllerdb/ControllerDbStore.h"
#include "source/store/ControllerDbPreferenceStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <string>

using namespace dish::controllerdb;
using dish::input::usbparse::HidParser;
using dish::source::controllerdb::catalogFromFiles;
using dish::source::controllerdb::readDbFolder;

namespace {

constexpr std::size_t kRecordsInDbVersion1 = 272;

CatalogPtr bundledCatalog() {
    return catalogFromFiles(readDbFolder(QStringLiteral(DISH_CONTROLLER_DB_DIR)),
                            DbSource::Bundled);
}

// The vendors HidrawGateway::isKnownFastLaneModel answers true for: Sony, Nintendo, 8BitDo.
constexpr std::array<std::uint16_t, 3> kFastLaneVendors{0x054C, 0x057E, 0x2DC8};

std::optional<Mapping> mappingOf(dish::input::usbparse::ButtonOrder order) {
    using dish::input::usbparse::ButtonOrder;
    switch (order) {
    case ButtonOrder::Western:
        return Mapping::Western;
    case ButtonOrder::Switch:
        return Mapping::Switch;
    case ButtonOrder::EightBitDo:
        return Mapping::EightBitDo;
    case ButtonOrder::PlayStationClassic:
        return Mapping::PlayStationClassic;
    }
    return std::nullopt;
}

std::optional<Family> familyOf(HidParser parser) {
    switch (parser) {
    case HidParser::GenericHid:
        return Family::Hid;
    case HidParser::SteamController:
        return Family::SteamController;
    case HidParser::SteamTriton:
        return Family::SteamTriton;
    case HidParser::DualShock4:
        return Family::DualShock4;
    case HidParser::DualSense:
        return Family::DualSense;
    case HidParser::SwitchProUsb:
        return Family::SwitchProUsb;
    case HidParser::None:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("bundled controller db: it verifies against its own manifest and holds 272 records",
          "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    CHECK(catalog->dbVersion() == 1);
    CHECK(catalog->db().records.size() == kRecordsInDbVersion1);
    CHECK(catalog->db().droppedRows == 0);
}

TEST_CASE("bundled controller db: every record has a unique id and an English name",
          "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    std::set<std::string> ids;
    for (const ControllerRecord& record : catalog->db().records) {
        CHECK(ids.insert(record.id).second);
        CHECK_FALSE(catalog->text(controllerNameKey(record.id), "en").empty());
    }
}

TEST_CASE("bundled controller db: no USB id is listed by two records", "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    std::set<std::uint32_t> seen;
    for (const ControllerRecord& record : catalog->db().records) {
        for (const DeviceId& id : record.usb) { CHECK(seen.insert(id.key()).second); }
    }
}

TEST_CASE("bundled controller db: the two PowerA ids kKnown listed twice are Xbox One records",
          "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    for (const DeviceId id : {DeviceId{0x24C6, 0x551A}, DeviceId{0x24C6, 0x561A}}) {
        const ControllerRecord* record = catalog->findUsb(id);
        REQUIRE(record != nullptr);
        CHECK(record->family == Family::XboxOneGip);
        CHECK(record->verification == Verification::Imported);
        CHECK(record->directRule == DirectRule::OptIn);
    }
}

TEST_CASE("bundled controller db: every compiled kKnownModels row has its record",
          "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    for (const auto& model : dish::input::usbparse::kKnownModels) {
        INFO(model.name);
        const DeviceId id{static_cast<std::uint16_t>(model.vid),
                          static_cast<std::uint16_t>(model.pid)};
        const ControllerRecord* record = catalog->findUsb(id);
        REQUIRE(record != nullptr);
        CHECK(std::optional<Family>(record->family) == familyOf(model.parser));
        if (model.parser == HidParser::GenericHid) {
            CHECK(record->family == Family::Hid);
            CHECK(std::optional<Mapping>(record->mapping) == mappingOf(model.order));
        }
        CHECK_FALSE(isInert(*record));
    }
}

TEST_CASE("bundled controller db: the fast-lane vendors are vendors of verified records",
          "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    for (const std::uint16_t vendor : kFastLaneVendors) {
        INFO(vendor);
        bool verifiedRecord = false;
        for (const ControllerRecord& record : catalog->db().records) {
            const bool ofVendor =
                std::any_of(record.usb.begin(), record.usb.end(),
                            [vendor](const DeviceId& id) { return id.vendorId == vendor; });
            verifiedRecord =
                verifiedRecord || (ofVendor && record.verification == Verification::Verified);
        }
        CHECK(verifiedRecord);
    }
}

TEST_CASE("bundled controller db: the DualSense is a live verified record with its Thor issue",
          "[controllerdb][bundled]") {
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    const ControllerRecord* ds5 = catalog->findUsb(DeviceId{0x054C, 0x0CE6});
    REQUIRE(ds5 != nullptr);
    CHECK(ds5->id == "sony-dualsense");
    CHECK(ds5->family == Family::DualSense);
    CHECK(ds5->verification == Verification::Verified);
    CHECK_FALSE(isInert(*ds5));
    REQUIRE(ds5->issues.size() == 1U);
    CHECK(ds5->issues[0].device == std::optional<std::string>("AYN Thor"));
    CHECK_FALSE(catalog->text(issueTitleKey(ds5->issues[0].id), "de").empty());
}

TEST_CASE("bundled controller db: a tampered file fails the manifest and is not used",
          "[controllerdb][bundled]") {
    auto files = readDbFolder(QStringLiteral(DISH_CONTROLLER_DB_DIR));
    REQUIRE(files.files.contains(QStringLiteral("strings/de.json")));
    files.files[QStringLiteral("strings/de.json")].append(' ');
    CHECK(catalogFromFiles(files, DbSource::Bundled) == nullptr);
}

TEST_CASE(
    "bundled controller db: the 2026 Steam Controller takes output reports and leaves OUT open",
    "[controllerdb][bundled]") {
    // dbVersion 1 as republished on 2026-11-19: Dish writes the pad's haptic rumble as an output
    // report, so its records promise the interrupt OUT route and leave the OUT endpoint unknown.
    const CatalogPtr catalog = bundledCatalog();
    REQUIRE(catalog != nullptr);
    for (const std::uint16_t pid : std::initializer_list<std::uint16_t>{0x1302, 0x1304, 0x1305}) {
        INFO(pid);
        const ControllerRecord* record = catalog->findUsb(DeviceId{0x28DE, pid});
        REQUIRE(record != nullptr);
        CHECK(record->family == Family::SteamTriton);
        CHECK(record->route == OutputRoute::InterruptOut);
        CHECK(record->expect.endpointOut == Expectation::Unknown);
    }
    const ControllerRecord* bluetooth = catalog->findBluetooth(DeviceId{0x28DE, 0x1303});
    REQUIRE(bluetooth != nullptr);
    CHECK(bluetooth->expect.endpointOut == Expectation::Unknown);
}

TEST_CASE("bundled controller db: the app's resource is the vendored folder, and it is in force",
          "[controllerdb][bundled]") {
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    dish::source::ControllerDbPreferenceStore prefs(std::make_unique<QSettings>(
        dir.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat));
    dish::source::controllerdb::ControllerDbStore store(
        &prefs, QString::fromLatin1(dish::source::controllerdb::kBundledDbDir), QString());
    store.load();
    REQUIRE(store.bundled() != nullptr);
    CHECK(store.bundled()->db() == bundledCatalog()->db());
    CHECK(store.active() == store.bundled());
    CHECK(store.state().value().activeVersion == 1);
    CHECK(store.state().value().source == DbSource::Bundled);
}
