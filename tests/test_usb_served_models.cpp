// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A served record drives how a Direct pad decodes (docs/controller-db.md sections 6.5 and 6.6),
// end to end through the manager over a gateway that pins each attach's model through the
// resolver the way HidrawGateway's scan does, and decodes what the pad sends with the pinned
// family the way its read loop does.

#include "source/usb/UsbGamepadManager.h"

#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/ControllerRecord.h"
#include "core/controllerdb/PadModel.h"
#include "core/controllerdb/Vocabulary.h"
#include "core/input/UsbReportParsers.h"
#include "core/reducer/DirectPadKey.h"
#include "source/store/UsbPathPreferenceStore.h"
#include "source/usb/UsbDeviceGateway.h"

#include "Input/GamepadInputProcessor.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dish::source::usb;
using dish::controllerdb::CatalogPtr;
using dish::controllerdb::ControllerCatalog;
using dish::controllerdb::ControllerDb;
using dish::controllerdb::ControllerRecord;
using dish::controllerdb::DbSource;
using dish::controllerdb::DeviceId;
using dish::controllerdb::DirectRule;
using dish::controllerdb::Family;
using dish::controllerdb::ModelPins;
using dish::controllerdb::PadModelResolver;
using dish::controllerdb::StringsTable;
using dish::controllerdb::Verification;
using dish::input::usbparse::HidParser;
using dish::source::UsbPathPreferenceRepository;
using dish::source::UsbPathPreferenceStore;
using dish::test::makeSharedSettings;
namespace usbparse = dish::input::usbparse;

namespace {

// An id no compiled table names (a Hori pad): the vendor fallback would read it as generic HID.
constexpr int kHoriVid = 0x0F0D;
constexpr int kHoriPid = 0x0055;
// A compiled kKnownModels row in the Switch button order.
constexpr int kPdpVid = 0x0E6F;
constexpr int kPdpPid = 0x0180;

ControllerRecord recordOf(const std::string& id, Family family, int vid, int pid) {
    ControllerRecord r;
    r.id = id;
    r.family = family;
    r.requiresFamilies = {std::string(dish::controllerdb::familyToken(family))};
    r.usb = {DeviceId{static_cast<std::uint16_t>(vid), static_cast<std::uint16_t>(pid)}};
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

// One pad on the bus, by the name of its current attach.
struct FakePad {
    int vendorId = 0;
    int productId = 0;
    std::string attach;
};

// A DualShock 4 USB report at rest but for Cross held and the left stick full right.
constexpr std::array<std::uint8_t, 10> kDs4CrossAndRight{0x01, 0xFF, 0x80, 0x80, 0x80,
                                                         0x28, 0x00, 0x00, 0x00, 0x00};

// Pins each attach through the resolver, as HidrawGateway's scan does, and decodes what the pad
// sends with the family its claim was handed, as its read loop does.
class ResolvingGateway : public UsbDeviceGateway {
  public:
    explicit ResolvingGateway(const PadModelResolver* resolver) : resolver_(resolver) {}

    std::vector<FakePad> pads;

    std::vector<UsbDeviceInfo> enumerate() override {
        std::vector<UsbDeviceInfo> out;
        std::vector<std::string> seen;
        for (const FakePad& pad : pads) {
            seen.push_back(pad.attach);
            UsbDeviceInfo d;
            d.vendorId = pad.vendorId;
            d.productId = pad.productId;
            d.name = "Pad";
            d.devicePath = "1-2";
            d.hasOutEndpoint = true;
            d.model = pins_.pinFor(pad.attach, pad.vendorId, pad.productId, resolver_);
            out.push_back(d);
        }
        pins_.keepOnly(seen);
        return out;
    }
    ClaimResult claim(const UsbDeviceInfo& device,
                      std::function<void(const UsbReport&)> onReport) override {
        claimedParser = device.model.parser;
        sticks_.productId = device.productId;
        onReport_ = std::move(onReport);
        return ClaimResult::success(nextSyntheticId_--);
    }
    void releaseClaim(int /*syntheticId*/) override {}
    bool isClaimAlive(int /*syntheticId*/) const override { return true; }
    std::int64_t completionCount(int /*syntheticId*/) const override { return 1; }
    std::int64_t decodedCount(int /*syntheticId*/) const override { return 1; }
    bool writeOutputReport(int /*syntheticId*/, const std::uint8_t* data,
                           std::size_t len) override {
        const std::lock_guard<std::mutex> lock(writesMtx_);
        writes_.emplace_back(data, data + len);
        return true;
    }

    // The read loop's decode of one report, published the way HidrawGateway's is.
    bool deliver(const std::uint8_t* raw, std::size_t len) {
        usbparse::ParsedReport parsed{};
        if (!usbparse::decodeReport(claimedParser, raw, len, parsed, sticks_)) { return false; }
        UsbReport report;
        report.wButtons = parsed.wButtons;
        report.lx = parsed.lx;
        report.ly = parsed.ly;
        onReport_(report);
        return true;
    }

    // The first OUT report the claim's worker wrote, waited for on its thread.
    std::optional<std::vector<std::uint8_t>> firstWrite() const {
        constexpr auto kPatience = std::chrono::seconds(2);
        const auto until = std::chrono::steady_clock::now() + kPatience;
        while (std::chrono::steady_clock::now() < until) {
            {
                const std::lock_guard<std::mutex> lock(writesMtx_);
                if (!writes_.empty()) { return writes_.front(); }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

    HidParser claimedParser = HidParser::None;

  private:
    const PadModelResolver* resolver_;
    ModelPins pins_;
    usbparse::StickAutoRangeState sticks_;
    std::function<void(const UsbReport&)> onReport_;
    int nextSyntheticId_ = -1000;
    mutable std::mutex writesMtx_;
    std::vector<std::vector<std::uint8_t>> writes_;
};

// The manager over the resolving gateway, recording what the processor was handed.
struct ServedRig {
    PadModelResolver resolver;
    ResolvingGateway gateway{&resolver};
    dish::input::GamepadInputProcessor processor;
    UsbPathPreferenceRepository repo{makeSharedSettings()};
    UsbPathPreferenceStore prefs{&repo};
    std::unique_ptr<UsbGamepadManager> manager;
    std::uint16_t publishedButtons = 0;
    std::int16_t publishedLx = 0;

    ServedRig() {
        processor.setReportSender([this](const std::string& /*id*/, std::uint16_t buttons,
                                         std::uint8_t /*lt*/, std::uint8_t /*rt*/, std::int16_t lx,
                                         std::int16_t /*ly*/, std::int16_t /*rx*/,
                                         std::int16_t /*ry*/, std::uint32_t /*ext*/) {
            publishedButtons = buttons;
            publishedLx = lx;
        });
        manager = std::make_unique<UsbGamepadManager>(&gateway, &processor, &prefs, nullptr);
    }

    void plugIn(int vid, int pid, const std::string& attach) {
        gateway.pads = {FakePad{vid, pid, attach}};
        manager->reconcile();
    }

    void unplug() {
        gateway.pads.clear();
        manager->reconcile();
        manager->reconcile();
    }

    static dish::reducer::DirectPadKey padOf(int vid, int pid) {
        return dish::reducer::DirectPadKey{vid, pid};
    }
};

} // namespace

TEST_CASE("served models: a served-only id goes Direct and decodes with the served family",
          "[usb-manager][padmodel]") {
    ServedRig rig;
    const CatalogPtr db =
        catalogOf(2, {recordOf("hori", Family::DualShock4, kHoriVid, kHoriPid)}, DbSource::Cache);
    rig.resolver.install(db, catalogOf(1, {}));
    rig.plugIn(kHoriVid, kHoriPid, "0003:0F0D:0055.0001");

    // The row is verified and its rule is auto, so the pad goes Direct without a pick.
    REQUIRE(rig.manager->isDirectClaimed(kHoriVid, kHoriPid));
    CHECK(rig.gateway.claimedParser == HidParser::DualShock4);
    REQUIRE(rig.gateway.deliver(kDs4CrossAndRight.data(), kDs4CrossAndRight.size()));
    CHECK(rig.publishedButtons == dish::input::GamepadInputProcessor::Buttons::kA);
    CHECK(rig.publishedLx > 30000);
}

TEST_CASE("served models: the capability solve, the claim and the output builder read one model",
          "[usb-manager][padmodel]") {
    ServedRig rig;
    rig.resolver.install(
        catalogOf(2, {recordOf("hori", Family::DualShock4, kHoriVid, kHoriPid)}, DbSource::Cache),
        catalogOf(1, {}));
    rig.plugIn(kHoriVid, kHoriPid, "0003:0F0D:0055.0001");
    REQUIRE(rig.manager->isDirectClaimed(kHoriVid, kHoriPid));

    const auto pad = ServedRig::padOf(kHoriVid, kHoriPid);
    const auto model = rig.manager->modelFor(pad);
    const auto target = rig.manager->directTarget(pad);
    REQUIRE(model.has_value());
    REQUIRE(target.has_value());
    CHECK(model->parser == HidParser::DualShock4);
    CHECK(target->parser == HidParser::DualShock4);
    CHECK(rig.gateway.claimedParser == HidParser::DualShock4);
    // The DualShock 4 builder writes its USB rumble report, id 0x05, which a generic HID pad has
    // no builder for.
    CHECK(rig.manager->applyRumble(*target, 0x8000, 0x4000));
    const auto written = rig.gateway.firstWrite();
    REQUIRE(written.has_value());
    CHECK(written->front() == 0x05);
}

TEST_CASE("served models: a database that lands while the pad is attached applies at its next "
          "attach",
          "[usb-manager][padmodel]") {
    ServedRig rig;
    rig.resolver.install(catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriVid, kHoriPid)}),
                         catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriVid, kHoriPid)}));
    rig.plugIn(kHoriVid, kHoriPid, "0003:0F0D:0055.0001");
    REQUIRE(rig.manager->isDirectClaimed(kHoriVid, kHoriPid));
    const auto pad = ServedRig::padOf(kHoriVid, kHoriPid);

    rig.resolver.install(
        catalogOf(2, {recordOf("hori", Family::DualSense, kHoriVid, kHoriPid)}, DbSource::Cache),
        catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriVid, kHoriPid)}));
    rig.manager->reconcile();
    CHECK(rig.manager->modelFor(pad)->parser == HidParser::DualShock4);
    CHECK(rig.manager->directTarget(pad)->parser == HidParser::DualShock4);
    CHECK(rig.gateway.claimedParser == HidParser::DualShock4);

    rig.unplug();
    CHECK_FALSE(rig.manager->modelFor(pad).has_value());
    rig.plugIn(kHoriVid, kHoriPid, "0003:0F0D:0055.0002");
    REQUIRE(rig.manager->isDirectClaimed(kHoriVid, kHoriPid));
    CHECK(rig.manager->modelFor(pad)->parser == HidParser::DualSense);
    CHECK(rig.gateway.claimedParser == HidParser::DualSense);
}

TEST_CASE("served models: a served row for an id the compiled tables name changes nothing",
          "[usb-manager][padmodel]") {
    ServedRig rig;
    rig.resolver.install(
        catalogOf(2, {recordOf("pdp", Family::DualSense, kPdpVid, kPdpPid)}, DbSource::Cache),
        catalogOf(1, {}));
    rig.plugIn(kPdpVid, kPdpPid, "0003:0E6F:0180.0001");
    const auto model = rig.manager->modelFor(ServedRig::padOf(kPdpVid, kPdpPid));
    REQUIRE(model.has_value());
    CHECK(model->parser == HidParser::GenericHid);
    CHECK(model->order == usbparse::ButtonOrder::Switch);
    // The compiled answer for a PDP pad is no fast lane: it waits for a pick, whatever the row
    // says.
    CHECK_FALSE(rig.manager->isDirectClaimed(kPdpVid, kPdpPid));
}

TEST_CASE("served models: an inert served row leaves the bundled row for its id in force",
          "[usb-manager][padmodel]") {
    ServedRig rig;
    ControllerRecord future = recordOf("hori", Family::DualSense, kHoriVid, kHoriPid);
    future.requiresOps = {"futureOp"};
    rig.resolver.install(catalogOf(2, {future}, DbSource::Cache),
                         catalogOf(1, {recordOf("hori", Family::DualShock4, kHoriVid, kHoriPid)}));
    rig.plugIn(kHoriVid, kHoriPid, "0003:0F0D:0055.0001");
    REQUIRE(rig.manager->isDirectClaimed(kHoriVid, kHoriPid));
    CHECK(rig.gateway.claimedParser == HidParser::DualShock4);
}
