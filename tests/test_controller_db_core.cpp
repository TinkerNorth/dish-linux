// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database's pure core (docs/controller-db.md): the vocabulary
// read both ways, the index and the strings fallback, which catalog is in
// force, what this build can apply, how a record classifies a pad the compiled
// tables do not name, the expect check, the issue match and the per-attach
// ledger. Every fixture is built in code; the real bundled file has its own
// test file.

#include "core/controllerdb/AttachedRecords.h"
#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/DirectModel.h"
#include "core/controllerdb/ExpectCheck.h"
#include "core/controllerdb/IssueMatch.h"
#include "core/controllerdb/Vocabulary.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dish::controllerdb;
using dish::input::usbparse::ButtonOrder;
using dish::input::usbparse::HidParser;

namespace {

constexpr DeviceId kDualSense{0x054C, 0x0CE6};
// An id no compiled table names: the vendor fallback would read it as generic HID.
constexpr DeviceId kThirdPartyPad{0x0F0D, 0x0055};

ControllerRecord recordOf(const std::string& id, Family family, std::vector<DeviceId> usb) {
    ControllerRecord r;
    r.id = id;
    r.family = family;
    r.requiresFamilies = {std::string(familyToken(family))};
    r.usb = std::move(usb);
    r.verification = Verification::Verified;
    return r;
}

std::shared_ptr<const ControllerCatalog> catalogOf(int dbVersion,
                                                   std::vector<ControllerRecord> records,
                                                   DbSource source = DbSource::Bundled) {
    ControllerDb db;
    db.dbVersion = dbVersion;
    db.records = std::move(records);
    std::map<std::string, StringsTable> strings;
    strings["en"] = {{"controller.sony-dualsense.name", "DualSense Wireless Controller"},
                     {"family.dualsense.name", "DualSense protocol"}};
    strings["de"] = {{"family.dualsense.name", "DualSense-Protokoll"}};
    return std::make_shared<const ControllerCatalog>(std::move(db), std::move(strings), source);
}

// The claimed interface's endpoints as the Direct path read them.
ObservedHardware observedOf(bool input, bool output) { return ObservedHardware{input, output}; }

} // namespace

TEST_CASE("controller db: every vocabulary word reads back as the token it came from",
          "[controllerdb]") {
    for (const auto& entry : kFamilyTokens) {
        CHECK(familyToken(familyFrom(entry.text)) == entry.text);
    }
    for (const auto& entry : kVerificationTokens) {
        CHECK(verificationToken(*fromToken(kVerificationTokens, entry.text)) == entry.text);
    }
    CHECK(mappingFrom("eightbitdo") == Mapping::EightBitDo);
    CHECK(initFrom("steamTritonQuiet") == InitKind::SteamTritonQuiet);
    CHECK(routeFrom("setReport") == OutputRoute::SetReport);
}

TEST_CASE("controller db: a word a later database adds reads as unknown, not as a failure",
          "[controllerdb]") {
    CHECK(familyFrom("joyConPair") == Family::Unknown);
    CHECK(mappingFrom("nintendoClassic") == Mapping::Unknown);
    CHECK(initFrom("dualSenseWake") == InitKind::Unknown);
    CHECK(routeFrom("bulkOut") == OutputRoute::Unknown);
}

TEST_CASE("controller db: a device id is lowercase vid:pid and nothing else", "[controllerdb]") {
    CHECK(parseDeviceId("054c:0ce6") == kDualSense);
    CHECK(deviceIdText(kDualSense) == "054c:0ce6");
    CHECK_FALSE(parseDeviceId("054C:0CE6").has_value());
    CHECK_FALSE(parseDeviceId("054c-0ce6").has_value());
    CHECK_FALSE(parseDeviceId("054c:0ce").has_value());
    CHECK_FALSE(parseDeviceId("").has_value());
}

TEST_CASE("controller db: the verification chip folds every unverified state into layout guessed",
          "[controllerdb]") {
    CHECK(verificationChipToken(Verification::Verified) == "verified");
    CHECK(verificationChipToken(Verification::Flagged) == "flagged");
    CHECK(verificationChipToken(Verification::Imported) == "guessed");
    CHECK(verificationChipToken(Verification::Crowd) == "guessed");
    CHECK(verificationChipToken(Verification::Guessed) == "guessed");
}

TEST_CASE("controller db: platforms.linux outranks the record's own direct rule",
          "[controllerdb]") {
    CHECK(directRuleFor(DirectRule::WhenOsCannot, DirectRule::Auto) == DirectRule::WhenOsCannot);
    CHECK(directRuleFor(std::nullopt, DirectRule::OptIn) == DirectRule::OptIn);
}

TEST_CASE("controller db: the index finds a record by its USB and its Bluetooth ids apart",
          "[controllerdb]") {
    ControllerRecord steam =
        recordOf("valve-steam-controller-bluetooth", Family::SteamControllerBle, {});
    steam.bluetooth = {DeviceId{0x28DE, 0x1106}};
    const auto catalog =
        catalogOf(1, {recordOf("sony-dualsense", Family::DualSense, {kDualSense}), steam});

    REQUIRE(catalog->findUsb(kDualSense) != nullptr);
    CHECK(catalog->findUsb(kDualSense)->id == "sony-dualsense");
    CHECK(catalog->findBluetooth(DeviceId{0x28DE, 0x1106})->id == steam.id);
    CHECK(catalog->findUsb(DeviceId{0x28DE, 0x1106}) == nullptr);
    CHECK(catalog->findBluetooth(kDualSense) == nullptr);
    CHECK(catalog->findById("sony-dualsense") != nullptr);
    CHECK(catalog->findById("no-such-record") == nullptr);
}

TEST_CASE("controller db: an id two records list resolves to the first, every time",
          "[controllerdb]") {
    const auto catalog = catalogOf(1, {recordOf("first", Family::Hid, {kThirdPartyPad}),
                                       recordOf("second", Family::Hid, {kThirdPartyPad})});
    CHECK(catalog->findUsb(kThirdPartyPad)->id == "first");
}

TEST_CASE("controller db: a string missing in the person's language falls back to English",
          "[controllerdb]") {
    const auto catalog = catalogOf(1, {});
    CHECK(catalog->text("family.dualsense.name", "de") == "DualSense-Protokoll");
    CHECK(catalog->text("controller.sony-dualsense.name", "de") == "DualSense Wireless Controller");
    CHECK(catalog->text("controller.sony-dualsense.name", "pt-BR") ==
          "DualSense Wireless Controller");
    CHECK(catalog->text("controller.unknown.name", "de").empty());
}

TEST_CASE("controller db: the strings locale is the first UI language the database has",
          "[controllerdb]") {
    CHECK(stringsLocaleFor({"de-DE", "en-US"}) == "de");
    CHECK(stringsLocaleFor({"de-AT"}) == "de");
    CHECK(stringsLocaleFor({"pt-BR"}) == "pt-BR");
    CHECK(stringsLocaleFor({"pt-PT"}) == "pt-BR");
    CHECK(stringsLocaleFor({"bs-Latn-BA"}) == "bs");
    CHECK(stringsLocaleFor({"fr-CA"}) == "fr");
    CHECK(stringsLocaleFor({"ja-JP", "fr-CA"}) == "fr");
    CHECK(stringsLocaleFor({"es-419"}) == "es");
    CHECK(stringsLocaleFor({"en-GB"}) == "en");
    CHECK(stringsLocaleFor({"ja-JP"}) == "en");
    CHECK(stringsLocaleFor({}) == "en");
}

TEST_CASE("controller db: a cached download is in force only while its version is greater",
          "[controllerdb]") {
    const auto bundled = catalogOf(3, {});
    SECTION("newer cache") {
        const auto cached = catalogOf(4, {}, DbSource::Cache);
        CHECK(chooseActive(bundled, cached) == cached);
    }
    SECTION("equal cache, after the app itself shipped that version") {
        const auto cached = catalogOf(3, {}, DbSource::Cache);
        CHECK(chooseActive(bundled, cached) == bundled);
    }
    SECTION("older cache") {
        const auto cached = catalogOf(2, {}, DbSource::Cache);
        CHECK(chooseActive(bundled, cached) == bundled);
    }
    SECTION("no cache") { CHECK(chooseActive(bundled, nullptr) == bundled); }
    SECTION("no bundled copy that verified") {
        const auto cached = catalogOf(1, {}, DbSource::Cache);
        CHECK(chooseActive(nullptr, cached) == cached);
    }
}

TEST_CASE("controller db: a record is inert when it needs what this build lacks",
          "[controllerdb]") {
    SECTION("a family this build parses is live") {
        CHECK_FALSE(isInert(recordOf("pad", Family::DualSense, {kDualSense})));
    }
    SECTION("the Xbox families are xpad's here") {
        CHECK(isInert(recordOf("pad", Family::Xinput360, {kThirdPartyPad})));
        CHECK(isInert(recordOf("pad", Family::XboxOneGip, {kThirdPartyPad})));
    }
    SECTION("a family a later database names") {
        ControllerRecord r = recordOf("pad", Family::Unknown, {kThirdPartyPad});
        r.requiresFamilies = {"joyConPair"};
        CHECK(isInert(r));
    }
    SECTION("a required family the build lacks, beside one it has") {
        ControllerRecord r = recordOf("pad", Family::Hid, {kThirdPartyPad});
        r.mapping = Mapping::Western;
        r.requiresFamilies.push_back("stadia");
        CHECK(isInert(r));
    }
    SECTION("any op at all: schema 1 defines none this build runs") {
        ControllerRecord r = recordOf("pad", Family::DualSense, {kDualSense});
        r.requiresOps = {"hapticsV2"};
        CHECK(isInert(r));
    }
    SECTION("a hid record with no button order it can decode by") {
        ControllerRecord r = recordOf("pad", Family::Hid, {kThirdPartyPad});
        r.mapping = Mapping::Unknown;
        CHECK(isInert(r));
        r.mapping = Mapping::None;
        CHECK(isInert(r));
        r.mapping = Mapping::Switch;
        CHECK_FALSE(isInert(r));
    }
    SECTION("an output route or init kind it has never heard of") {
        ControllerRecord r = recordOf("pad", Family::DualSense, {kDualSense});
        r.route = OutputRoute::Unknown;
        CHECK(isInert(r));
        r.route = OutputRoute::InterruptOut;
        r.init = InitKind::Unknown;
        CHECK(isInert(r));
    }
}

TEST_CASE("controller db: an inert served row leaves the bundled row for the same id in force",
          "[controllerdb]") {
    const ControllerRecord bundledRow = recordOf("pad", Family::DualShock4, {kThirdPartyPad});
    const auto bundled = catalogOf(1, {bundledRow});
    ControllerRecord served = bundledRow;
    served.requiresOps = {"futureOp"};

    CHECK(directRecordFor(&served, *bundled) == bundled->findById("pad"));
    SECTION("a live served row is the one in force") {
        const ControllerRecord live = recordOf("pad", Family::DualSense, {kThirdPartyPad});
        CHECK(directRecordFor(&live, *bundled) == &live);
    }
    SECTION("an inert row with no bundled twin applies nothing") {
        const ControllerRecord lonely = recordOf("lonely", Family::Stadia, {kThirdPartyPad});
        CHECK(directRecordFor(&lonely, *bundled) == nullptr);
    }
    SECTION("no row at all") { CHECK(directRecordFor(nullptr, *bundled) == nullptr); }
    SECTION("a flagged served row gives way to the live bundled row, and applies without one") {
        ControllerRecord flagged = recordOf("pad", Family::DualSense, {kThirdPartyPad});
        flagged.verification = Verification::Flagged;
        CHECK(directRecordFor(&flagged, *bundled) == bundled->findById("pad"));
        const auto empty = catalogOf(1, {});
        CHECK(directRecordFor(&flagged, *empty) == &flagged);
    }
}

TEST_CASE("controller db: the compiled tables outrank a record for an id they name",
          "[controllerdb]") {
    // The PDP Faceoff is a kKnownModels row in the Switch order; a record claiming otherwise
    // does not move it.
    ControllerRecord r = recordOf("pdp", Family::Hid, {DeviceId{0x0E6F, 0x0180}});
    r.mapping = Mapping::Western;
    const DirectModel model = resolveDirectModel(0x0E6F, 0x0180, &r);
    CHECK(model.source == ModelSource::Compiled);
    CHECK(model.parser == HidParser::GenericHid);
    CHECK(model.order == ButtonOrder::Switch);

    SECTION("the Sony pads named by product are compiled too") {
        const ControllerRecord hid = recordOf("ds5", Family::Hid, {kDualSense});
        const DirectModel ds5 = resolveDirectModel(0x054C, 0x0CE6, &hid);
        CHECK(ds5.source == ModelSource::Compiled);
        CHECK(ds5.parser == HidParser::DualSense);
    }
}

TEST_CASE("controller db: a live record classifies a pad the compiled tables lack",
          "[controllerdb]") {
    const ControllerRecord hori =
        recordOf("hori-fighting-commander-ps4", Family::DualShock4, {kThirdPartyPad});
    const DirectModel model = resolveDirectModel(0x0F0D, 0x0055, &hori);
    CHECK(model == DirectModel{HidParser::DualShock4, ButtonOrder::Western, ModelSource::Database});

    SECTION("a hid record brings its button order") {
        ControllerRecord hid = recordOf("eightbitdo", Family::Hid, {kThirdPartyPad});
        hid.mapping = Mapping::EightBitDo;
        CHECK(resolveDirectModel(0x0F0D, 0x0055, &hid) ==
              DirectModel{HidParser::GenericHid, ButtonOrder::EightBitDo, ModelSource::Database});
    }
}

TEST_CASE("controller db: an inert record or none leaves the vendor fallback", "[controllerdb]") {
    const ControllerRecord xbox = recordOf("xbox", Family::Xinput360, {kThirdPartyPad});
    CHECK(resolveDirectModel(0x0F0D, 0x0055, &xbox) ==
          DirectModel{HidParser::GenericHid, ButtonOrder::Western, ModelSource::Fallback});
    CHECK(resolveDirectModel(0x0F0D, 0x0055, nullptr).source == ModelSource::Fallback);
    // A Nintendo pad no table names keeps the vendor guess the binary makes today.
    CHECK(resolveDirectModel(0x057E, 0x2009, nullptr).parser == HidParser::SwitchProUsb);
}

TEST_CASE("controller db: the expect check names each disagreement in the contract's form",
          "[controllerdb]") {
    const Expect both{Expectation::Present, Expectation::Present};
    SECTION("all as expected") { CHECK(expectMismatches(both, observedOf(true, true)).empty()); }
    SECTION("an OUT endpoint the record promises and the pad lacks") {
        CHECK(expectMismatches(both, observedOf(true, false)) ==
              std::vector<std::string>{"endpoints.out: expected true, found false"});
    }
    SECTION("an OUT endpoint the record rules out") {
        const Expect noOut{Expectation::Present, Expectation::Absent};
        CHECK(expectMismatches(noOut, observedOf(true, true)) ==
              std::vector<std::string>{"endpoints.out: expected false, found true"});
    }
    SECTION("both wrong, IN first") {
        CHECK(expectMismatches(both, observedOf(false, false)) ==
              std::vector<std::string>{"endpoints.in: expected true, found false",
                                       "endpoints.out: expected true, found false"});
    }
    SECTION("an expectation the maintainer left null is not compared") {
        const Expect unknownOut{Expectation::Present, Expectation::Unknown};
        CHECK(expectMismatches(unknownOut, observedOf(true, false)).empty());
    }
    SECTION("a side the path did not observe is not compared") {
        CHECK(expectMismatches(both, ObservedHardware{true, std::nullopt}).empty());
    }
}

TEST_CASE("controller db: an issue shows for its transport on its machine and system",
          "[controllerdb]") {
    Issue issue;
    issue.id = "thor-ds5-rumble-usb";
    issue.transport = Transport::Usb;
    issue.status = "open";
    const HostFacts thor{"AYN Thor", 6};

    SECTION("only an open issue shows; fixed and wontfix are history") {
        issue.status = "fixed";
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
        issue.status = "wontfix";
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
    }

    SECTION("no device and no os: every machine") {
        CHECK(issueApplies(issue, Transport::Usb, thor));
        CHECK(issueApplies(issue, Transport::Usb, HostFacts{"Dell XPS 13", 5}));
    }
    SECTION("another transport") { CHECK_FALSE(issueApplies(issue, Transport::Bluetooth, thor)); }
    SECTION("its device, named without regard to case or spacing") {
        issue.device = "AYN Thor";
        CHECK(issueApplies(issue, Transport::Usb, HostFacts{"  ayn thor ", 6}));
        CHECK_FALSE(issueApplies(issue, Transport::Usb, HostFacts{"Dell XPS 13", 6}));
    }
    SECTION("an os map that names only Android never matches Linux") {
        issue.os = {IssueOs{"android", "15"}};
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
    }
    SECTION("a linux entry matches its own kernel major version only") {
        issue.os = {IssueOs{"android", "15"}, IssueOs{"linux", "6"}};
        CHECK(issueApplies(issue, Transport::Usb, thor));
        CHECK_FALSE(issueApplies(issue, Transport::Usb, HostFacts{"AYN Thor", 5}));
    }
    SECTION("a linux version given as a string is read by its major number") {
        issue.os = {IssueOs{"linux", "6.8"}};
        CHECK(issueApplies(issue, Transport::Usb, thor));
        issue.os = {IssueOs{"linux", "5"}};
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
        issue.os = {IssueOs{"linux", "latest"}};
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
    }
    SECTION("windows and macos are other platforms' keys") {
        issue.os = {IssueOs{"windows", "6"}};
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
        issue.os = {IssueOs{"macos", "6"}};
        CHECK_FALSE(issueApplies(issue, Transport::Usb, thor));
    }
    SECTION("applicableIssues keeps the matching ones in order") {
        ControllerRecord r = recordOf("sony-dualsense", Family::DualSense, {kDualSense});
        Issue bluetooth = issue;
        bluetooth.id = "bt-only";
        bluetooth.transport = Transport::Bluetooth;
        r.issues = {issue, bluetooth};
        const std::vector<Issue> shown = applicableIssues(r, Transport::Usb, thor);
        REQUIRE(shown.size() == 1U);
        CHECK(shown[0].id == "thor-ds5-rumble-usb");
    }
}

TEST_CASE("controller db: a pad keeps the record it was matched to at attach", "[controllerdb]") {
    ControllerRecord v1 = recordOf("sony-dualsense", Family::DualSense, {kDualSense});
    v1.expect = Expect{Expectation::Present, Expectation::Present};
    const auto first = catalogOf(1, {v1});
    const PadAttach direct{"direct:ds5", kDualSense, false, observedOf(true, false)};
    const PadAttach standard{"sdl:3", kDualSense, false, std::nullopt};

    const AttachedIdentities attached = reconcileAttached({}, {direct, standard}, *first);
    REQUIRE(attached.size() == 2U);
    CHECK(attached.at("direct:ds5").record->id == "sony-dualsense");
    CHECK(attached.at("direct:ds5").mismatches ==
          std::vector<std::string>{"endpoints.out: expected true, found false"});
    SECTION("only the Direct path observes, so only it is checked") {
        CHECK(attached.at("sdl:3").mismatches.empty());
    }
    SECTION("a database that lands while it is attached applies at its next attach") {
        ControllerRecord v2 = v1;
        v2.id = "sony-dualsense-renamed";
        const auto second = catalogOf(2, {v2});
        const AttachedIdentities still = reconcileAttached(attached, {direct}, *second);
        CHECK(still.at("direct:ds5").record->id == "sony-dualsense");
        const AttachedIdentities gone = reconcileAttached(still, {}, *second);
        CHECK(gone.empty());
        const AttachedIdentities back = reconcileAttached(gone, {direct}, *second);
        CHECK(back.at("direct:ds5").record->id == "sony-dualsense-renamed");
    }
    SECTION("a pad no record lists is attached with none") {
        const PadAttach unknown{"sdl:9", kThirdPartyPad, false, std::nullopt};
        const AttachedIdentities none = reconcileAttached({}, {unknown}, *first);
        CHECK_FALSE(none.at("sdl:9").record.has_value());
    }
    SECTION("a Bluetooth pad is looked up by its Bluetooth ids") {
        const PadAttach bluetooth{"sdl:4", kDualSense, true, std::nullopt};
        CHECK_FALSE(reconcileAttached({}, {bluetooth}, *first).at("sdl:4").record.has_value());
    }
    SECTION("an inert record is matched and says so") {
        const auto xbox = catalogOf(1, {recordOf("xbox", Family::Xinput360, {kThirdPartyPad})});
        const PadAttach pad{"sdl:5", kThirdPartyPad, false, std::nullopt};
        CHECK(reconcileAttached({}, {pad}, *xbox).at("sdl:5").inert);
    }
}
