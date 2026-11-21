// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The model an attach pins for a pad (core/controllerdb/PadModel.h): the compiled tables first,
// then the row in force, then the vendor fallback, with the per-product answers that follow from
// the family, and the resolver the composition root swaps whole when the database changes.

#include "core/controllerdb/PadModel.h"

#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/ControllerRecord.h"
#include "core/controllerdb/Vocabulary.h"
#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dish::controllerdb;
using dish::input::usbparse::ButtonOrder;
using dish::input::usbparse::HidParser;
using dish::input::usbparse::SteamTrackpads;
namespace usbparse = dish::input::usbparse;

namespace {

// Ids no compiled table names: the vendor fallback reads both as generic HID.
constexpr DeviceId kHoriPad{0x0F0D, 0x0055};
constexpr DeviceId kNewSteamPad{0x28DE, 0x1999};
// A Switch Pro: Nintendo is a vendor guess, not a table, so its record speaks for it.
constexpr DeviceId kSwitchPro{0x057E, 0x2009};
// A compiled kKnownModels row in the Switch button order.
constexpr DeviceId kPdpFaceoff{0x0E6F, 0x0180};

ControllerRecord recordOf(const std::string& id, Family family, DeviceId usb) {
    ControllerRecord r;
    r.id = id;
    r.family = family;
    r.requiresFamilies = {std::string(familyToken(family))};
    r.usb = {usb};
    r.verification = Verification::Verified;
    r.directRule = DirectRule::Auto;
    return r;
}

CatalogPtr catalogOf(int dbVersion, std::vector<ControllerRecord> records,
                     DbSource source = DbSource::Bundled) {
    ControllerDb db;
    db.dbVersion = dbVersion;
    db.records = std::move(records);
    return std::make_shared<const ControllerCatalog>(std::move(db),
                                                     std::map<std::string, StringsTable>{}, source);
}

PadModel modelOf(DeviceId id, const CatalogPtr& active, const CatalogPtr& bundled) {
    return padModelFor(id.vendorId, id.productId, active.get(), bundled.get());
}

} // namespace

TEST_CASE("pad model: a served row for an id the compiled tables name changes nothing",
          "[controllerdb][padmodel]") {
    ControllerRecord claim = recordOf("pdp", Family::DualShock4, kPdpFaceoff);
    claim.init = InitKind::SteamQuiet;
    const CatalogPtr served = catalogOf(2, {claim}, DbSource::Cache);
    const PadModel model = modelOf(kPdpFaceoff, served, catalogOf(1, {}));
    CHECK(model == modelWithoutDatabase(kPdpFaceoff.vendorId, kPdpFaceoff.productId));
    CHECK(model.source == ModelSource::Compiled);
    CHECK(model.parser == HidParser::GenericHid);
    CHECK(model.order == ButtonOrder::Switch);

    SECTION("a Sony pad named by product is compiled too") {
        const DeviceId dualSense{0x054C, 0x0CE6};
        const CatalogPtr hid = catalogOf(2, {recordOf("ds5", Family::Hid, dualSense)});
        CHECK(modelOf(dualSense, hid, nullptr).parser == HidParser::DualSense);
        CHECK(modelOf(dualSense, hid, nullptr).source == ModelSource::Compiled);
    }
    SECTION("a receiver keeps its slots") {
        const DeviceId dongle{0x28DE, 0x1142};
        const CatalogPtr dsRow = catalogOf(2, {recordOf("dongle", Family::DualShock4, dongle)});
        const PadModel receiver = modelOf(dongle, dsRow, nullptr);
        CHECK(receiver.isReceiver());
        CHECK(receiver.parser == HidParser::SteamController);
    }
}

TEST_CASE("pad model: a served row decodes an id the compiled tables lack with its own family",
          "[controllerdb][padmodel]") {
    ControllerRecord hori = recordOf("hori", Family::DualShock4, kHoriPad);
    const PadModel model = modelOf(kHoriPad, catalogOf(2, {hori}, DbSource::Cache), nullptr);
    CHECK(model.source == ModelSource::Database);
    CHECK(model.parser == HidParser::DualShock4);
    CHECK(model.order == ButtonOrder::Western);
    CHECK(model.compiledName == nullptr);
    CHECK_FALSE(model.isReceiver());

    SECTION("a hid row brings its button order") {
        ControllerRecord hid = recordOf("eightbitdo", Family::Hid, kHoriPad);
        hid.mapping = Mapping::EightBitDo;
        const PadModel eight = modelOf(kHoriPad, catalogOf(2, {hid}), nullptr);
        CHECK(eight.parser == HidParser::GenericHid);
        CHECK(eight.order == ButtonOrder::EightBitDo);
    }
    SECTION("the init kind is the row's") {
        ControllerRecord steam = recordOf("steam", Family::SteamController, kNewSteamPad);
        steam.init = InitKind::SteamQuiet;
        CHECK(modelOf(kNewSteamPad, catalogOf(2, {steam}), nullptr).init == InitKind::SteamQuiet);
    }
    SECTION("no row at all leaves the vendor fallback") {
        const PadModel none = modelOf(kHoriPad, catalogOf(2, {}), nullptr);
        CHECK(none.source == ModelSource::Fallback);
        CHECK(none.parser == HidParser::GenericHid);
        CHECK(none == modelWithoutDatabase(kHoriPad.vendorId, kHoriPad.productId));
    }
}

TEST_CASE("pad model: an inert served row leaves the bundled row for the same id in force",
          "[controllerdb][padmodel]") {
    const ControllerRecord bundledRow = recordOf("hori", Family::DualShock4, kHoriPad);
    ControllerRecord served = recordOf("hori", Family::DualSense, kHoriPad);
    served.requiresOps = {"futureOp"};
    const CatalogPtr bundled = catalogOf(1, {bundledRow});
    const CatalogPtr active = catalogOf(2, {served}, DbSource::Cache);

    const PadModel model = modelOf(kHoriPad, active, bundled);
    CHECK(model.source == ModelSource::Database);
    CHECK(model.parser == HidParser::DualShock4);

    SECTION("with no bundled twin the vendor fallback applies") {
        const PadModel lonely = modelOf(kHoriPad, active, catalogOf(1, {}));
        CHECK(lonely.source == ModelSource::Fallback);
        CHECK(lonely.parser == HidParser::GenericHid);
    }
    SECTION("with no bundled catalog an inert row applies nothing") {
        CHECK(modelOf(kHoriPad, active, nullptr).source == ModelSource::Fallback);
    }
}

TEST_CASE("pad model: a flagged served row gives way to the bundled row for the same id",
          "[controllerdb][padmodel]") {
    const ControllerRecord bundledRow = recordOf("hori", Family::DualShock4, kHoriPad);
    ControllerRecord flagged = recordOf("hori", Family::Hid, kHoriPad);
    flagged.mapping = Mapping::Western;
    flagged.verification = Verification::Flagged;
    const CatalogPtr active = catalogOf(2, {flagged}, DbSource::Cache);

    CHECK(modelOf(kHoriPad, active, catalogOf(1, {bundledRow})).parser == HidParser::DualShock4);
    SECTION("a flagged row with no bundled twin still applies") {
        const PadModel lone = modelOf(kHoriPad, active, catalogOf(1, {}));
        CHECK(lone.source == ModelSource::Database);
        CHECK(lone.parser == HidParser::GenericHid);
    }
}

TEST_CASE("pad model: a served row's per-product answers follow from its family",
          "[controllerdb][padmodel]") {
    SECTION("a Steam family: extended buttons, its trackpads, no framework gamepad") {
        ControllerRecord steam = recordOf("steam", Family::SteamTriton, kNewSteamPad);
        steam.input.extendedButtons = false;
        steam.input.trackpads = 0;
        const PadModel model = modelOf(kNewSteamPad, catalogOf(2, {steam}), nullptr);
        CHECK(model.extendedButtons);
        CHECK(model.steamTrackpads == SteamTrackpads::Pad2026);
        CHECK_FALSE(model.frameworkGamepadExpected);
        CHECK_FALSE(model.isReceiver());
    }
    SECTION("a Switch Pro: its Capture button, whatever the row's capabilities say") {
        ControllerRecord pro = recordOf("switch-pro", Family::SwitchProUsb, kSwitchPro);
        pro.input.extendedButtons = false;
        const PadModel model = modelOf(kSwitchPro, catalogOf(2, {pro}), nullptr);
        CHECK(model.source == ModelSource::Database);
        CHECK(model.extendedButtons);
        CHECK(model.frameworkGamepadExpected);
    }
    SECTION("a DualShock 4 family: none of them") {
        const PadModel model = modelOf(
            kHoriPad, catalogOf(2, {recordOf("hori", Family::DualShock4, kHoriPad)}), nullptr);
        CHECK_FALSE(model.extendedButtons);
        CHECK(model.steamTrackpads == SteamTrackpads::None);
        CHECK(model.frameworkGamepadExpected);
    }
}

TEST_CASE("pad model: a row's Direct rule decides whether the pad goes Direct on its own",
          "[controllerdb][padmodel]") {
    CHECK(autoClaimsByRule(DirectRule::Auto, Verification::Verified, true));
    CHECK_FALSE(autoClaimsByRule(DirectRule::Auto, Verification::Imported, true));
    CHECK_FALSE(autoClaimsByRule(DirectRule::OptIn, Verification::Verified, true));
    CHECK_FALSE(autoClaimsByRule(DirectRule::WhenOsCannot, Verification::Verified, true));
    CHECK(autoClaimsByRule(DirectRule::WhenOsCannot, Verification::Verified, false));

    SECTION("a verified auto row claims a pad no vendor fast lane covers") {
        CHECK(modelOf(kHoriPad, catalogOf(2, {recordOf("hori", Family::DualShock4, kHoriPad)}),
                      nullptr)
                  .autoClaims);
    }
    SECTION("an opt-in row outranks the vendor's fast lane") {
        const DeviceId sonyClone{0x054C, 0x05C5};
        ControllerRecord imported = recordOf("ds4-clone", Family::DualShock4, sonyClone);
        imported.directRule = DirectRule::OptIn;
        imported.verification = Verification::Imported;
        CHECK(modelWithoutDatabase(sonyClone.vendorId, sonyClone.productId).autoClaims);
        CHECK_FALSE(modelOf(sonyClone, catalogOf(2, {imported}), nullptr).autoClaims);
    }
}

TEST_CASE("pad model: with no database an attach pins what the compiled tables answer",
          "[controllerdb][padmodel]") {
    std::vector<DeviceId> ids;
    for (const auto& known : usbparse::kKnownModels) {
        ids.push_back(
            DeviceId{static_cast<std::uint16_t>(known.vid), static_cast<std::uint16_t>(known.pid)});
    }
    // The Sony pads named by product, a Switch Pro and a Joy-Con grip (the vendor's guess), the
    // Edge, and two pads nothing names.
    constexpr std::array<std::uint16_t, 6> kSonyPids{0x05C4, 0x09CC, 0x0BA0,
                                                     0x0CE6, 0x0DF2, 0x05C5};
    for (const std::uint16_t pid : kSonyPids) { ids.push_back(DeviceId{0x054C, pid}); }
    ids.push_back(kSwitchPro);
    ids.push_back(DeviceId{0x057E, 0x200E});
    ids.push_back(kHoriPad);
    ids.push_back(DeviceId{0x2DC8, 0x6001});
    for (const DeviceId& id : ids) {
        const int vid = id.vendorId;
        const int pid = id.productId;
        const PadModel model = modelWithoutDatabase(vid, pid);
        INFO(deviceIdText(id));
        CHECK(model == padModelFor(vid, pid, nullptr, nullptr));
        CHECK(model.parser == usbparse::parserForDevice(vid, pid));
        CHECK(model.order == usbparse::buttonOrderForDevice(vid, pid));
        CHECK(model.receiverFirstSlot == usbparse::receiverSlotsFor(vid, pid).first);
        CHECK(model.receiverLastSlot == usbparse::receiverSlotsFor(vid, pid).last);
        CHECK(model.extendedButtons == usbparse::modelHasExtendedButtons(vid, pid));
        CHECK(model.steamTrackpads == usbparse::modelSteamTrackpads(vid, pid));
        CHECK(model.frameworkGamepadExpected == usbparse::modelExpectsFrameworkGamepad(vid, pid));
        CHECK(model.autoClaims == vendorFastLane(vid));
        const auto* known = usbparse::lookupKnownModel(vid, pid);
        CHECK(model.compiledName == (known != nullptr ? known->name : nullptr));
    }
    CHECK(vendorFastLane(0x054C));
    CHECK(vendorFastLane(0x057E));
    CHECK(vendorFastLane(0x2DC8));
    CHECK_FALSE(vendorFastLane(0x0F0D));
}

TEST_CASE("pad model: each family's compiled init sequence, in the database's words",
          "[controllerdb][padmodel]") {
    CHECK(initOfFamily(HidParser::SteamController) == InitKind::SteamQuiet);
    CHECK(initOfFamily(HidParser::SteamTriton) == InitKind::SteamTritonQuiet);
    CHECK(initOfFamily(HidParser::SwitchProUsb) == InitKind::SwitchProHandshake);
    CHECK(initOfFamily(HidParser::DualSense) == InitKind::None);
    CHECK(initOfFamily(HidParser::DualShock4) == InitKind::None);
    CHECK(initOfFamily(HidParser::GenericHid) == InitKind::None);
    CHECK(modelWithoutDatabase(0x28DE, 0x1102).init == InitKind::SteamQuiet);
}

TEST_CASE("pad model resolver: it answers from the snapshot installed last, replaced whole",
          "[controllerdb][padmodel]") {
    PadModelResolver resolver;
    CHECK(resolver.resolve(kHoriPad.vendorId, kHoriPad.productId) ==
          modelWithoutDatabase(kHoriPad.vendorId, kHoriPad.productId));

    const CatalogPtr bundled = catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriPad)});
    resolver.install(bundled, bundled);
    const PadModel attached = resolver.resolve(kHoriPad.vendorId, kHoriPad.productId);
    CHECK(attached.parser == HidParser::DualShock4);

    SECTION("a later database without the row takes the bundled one back, not a merge") {
        ControllerRecord other = recordOf("other", Family::DualSense, kNewSteamPad);
        resolver.install(catalogOf(2, {other}, DbSource::Cache), catalogOf(1, {}));
        CHECK(resolver.resolve(kHoriPad.vendorId, kHoriPad.productId).source ==
              ModelSource::Fallback);
        CHECK(resolver.resolve(kNewSteamPad.vendorId, kNewSteamPad.productId).parser ==
              HidParser::DualSense);
        CHECK(attached.parser == HidParser::DualShock4);
    }
}

namespace {

// Resolves on its own thread until told to stop, counting answers that are neither database's.
struct ResolvingThread {
    explicit ResolvingThread(const PadModelResolver& resolver)
        : thread_(&ResolvingThread::run, this, std::cref(resolver)) {}
    ~ResolvingThread() {
        stop_.store(true);
        thread_.join();
    }
    ResolvingThread(const ResolvingThread&) = delete;
    ResolvingThread& operator=(const ResolvingThread&) = delete;
    ResolvingThread(ResolvingThread&&) = delete;
    ResolvingThread& operator=(ResolvingThread&&) = delete;

    int strays() const { return strays_.load(); }
    int answers() const { return answers_.load(); }

  private:
    void run(const PadModelResolver& resolver) {
        while (!stop_.load()) {
            const HidParser parser = resolver.resolve(kHoriPad.vendorId, kHoriPad.productId).parser;
            const bool known = parser == HidParser::DualShock4 || parser == HidParser::DualSense;
            if (!known) { strays_.fetch_add(1); }
            answers_.fetch_add(1);
        }
    }

    std::atomic<bool> stop_{false};
    std::atomic<int> strays_{0};
    std::atomic<int> answers_{0};
    std::thread thread_;
};

} // namespace

TEST_CASE("pad model resolver: a resolve on another thread meets a whole snapshot during swaps",
          "[controllerdb][padmodel]") {
    PadModelResolver resolver;
    const CatalogPtr ds4 = catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriPad)});
    const CatalogPtr ds5 = catalogOf(2, {recordOf("hori", Family::DualSense, kHoriPad)});
    resolver.install(ds4, ds4);
    const ResolvingThread reader(resolver);
    constexpr int kSwaps = 200;
    for (int i = 0; i < kSwaps; ++i) {
        const CatalogPtr& next = i % 2 == 0 ? ds5 : ds4;
        resolver.install(next, ds4);
    }
    while (reader.answers() == 0) { std::this_thread::yield(); }
    CHECK(reader.strays() == 0);
}

TEST_CASE("model pins: a database that lands while a pad is attached applies at its next attach",
          "[controllerdb][padmodel]") {
    PadModelResolver resolver;
    const CatalogPtr ds4 = catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriPad)});
    resolver.install(ds4, ds4);
    ModelPins pins;
    const std::string attach = "0003:0F0D:0055.0007";
    CHECK(pins.pinFor(attach, kHoriPad.vendorId, kHoriPad.productId, &resolver).parser ==
          HidParser::DualShock4);

    resolver.install(catalogOf(2, {recordOf("hori", Family::DualSense, kHoriPad)}, DbSource::Cache),
                     ds4);
    pins.keepOnly({attach});
    CHECK(pins.pinFor(attach, kHoriPad.vendorId, kHoriPad.productId, &resolver).parser ==
          HidParser::DualShock4);

    SECTION("the pad comes back under a new name and reads the database in force") {
        pins.keepOnly({});
        CHECK(pins.size() == 0);
        const std::string again = "0003:0F0D:0055.0008";
        CHECK(pins.pinFor(again, kHoriPad.vendorId, kHoriPad.productId, &resolver).parser ==
              HidParser::DualSense);
    }
    SECTION("a second pad of the model that attaches now reads it too") {
        CHECK(pins.pinFor("0003:0F0D:0055.0009", kHoriPad.vendorId, kHoriPad.productId, &resolver)
                  .parser == HidParser::DualSense);
        CHECK(pins.size() == 2);
    }
    SECTION("with no resolver an attach pins the compiled answer") {
        CHECK(pins.pinFor("0003:0F0D:0055.000A", kHoriPad.vendorId, kHoriPad.productId, nullptr) ==
              modelWithoutDatabase(kHoriPad.vendorId, kHoriPad.productId));
    }
}
