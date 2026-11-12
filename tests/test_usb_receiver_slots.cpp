// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A Steam Controller wireless receiver on the USB-direct path, over a fake gateway. A receiver
// serves up to four paired controllers under one vid:pid, one HID interface each: the 2015
// receiver's slots are interfaces 1 to 4 (SDL's HIDAPI_DriverSteam_IsSupportedDevice, Linux's
// steam_is_valve_interface), the 2026 receivers' 2 to 5
// (HIDAPI_DriverSteamTriton_IsSupportedDevice).

#include "source/usb/UsbGamepadManager.h"

#include "Input/GamepadInputProcessor.h"
#include "source/store/UsbPathPreferenceStore.h"
#include "source/usb/UsbDeviceGateway.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace dish::source::usb;
using dish::input::usbparse::kPadBatteryStatusDischarging;
using dish::reducer::DirectClaimFailure;
using dish::reducer::DirectPadKey;
using dish::reducer::PathChoice;
using dish::reducer::UsbPhase;
using dish::source::UsbPathPreferenceRepository;
using dish::source::UsbPathPreferenceStore;
using dish::test::makeSharedSettings;

namespace {

constexpr int kValve = 0x28DE;
constexpr int kReceiver2015 = 0x1142;
constexpr int kProteusReceiver = 0x1304;
constexpr int kNereidReceiver = 0x1305;
constexpr int kFirstSlot2015 = 1;
constexpr int kFirstSlot2026 = 2;
constexpr int kWiredSteamController = 0x1102;

UsbDeviceInfo slotDevice(int productId, int interfaceNumber) {
    UsbDeviceInfo d;
    d.vendorId = kValve;
    d.productId = productId;
    d.name = "Valve Steam Controller (dongle)";
    d.interfaceNumber = interfaceNumber;
    return d;
}

// The interfaces `slotCount` controllers ride, from the receiver's first slot on.
std::vector<int> slotInterfaces(int firstSlot, int slotCount) {
    std::vector<int> out(static_cast<std::size_t>(slotCount));
    std::iota(out.begin(), out.end(), firstSlot);
    return out;
}

UsbReport batteryReport(std::uint8_t level) {
    UsbReport r;
    r.batteryValid = true;
    r.batteryLevel = level;
    r.batteryStatus = kPadBatteryStatusDischarging;
    return r;
}

UsbReport controllerLeftEvent() {
    UsbReport r;
    r.linkEvent = PadLinkEvent::ControllerLeft;
    return r;
}

UsbReport controllerArrivedEvent() {
    UsbReport r;
    r.linkEvent = PadLinkEvent::ControllerArrived;
    return r;
}

// What a slot's output worker wrote to the pad, and from which thread.
struct SlotWrites {
    mutable std::mutex mtx;
    std::vector<std::vector<std::uint8_t>> features;
    std::vector<std::thread::id> featureWriters;
    std::vector<std::vector<std::uint8_t>> reports;

    std::size_t featureCount() const {
        const std::lock_guard<std::mutex> lock(mtx);
        return features.size();
    }
    // The 2015 pad's message ids, byte 1 of each feature report.
    std::vector<std::uint8_t> messages() const {
        const std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::uint8_t> out;
        for (const auto& feature : features) { out.push_back(feature.size() > 1 ? feature[1] : 0); }
        return out;
    }
    bool wroteOn(std::thread::id thread) const {
        const std::lock_guard<std::mutex> lock(mtx);
        return std::find(featureWriters.begin(), featureWriters.end(), thread) !=
               featureWriters.end();
    }
};

class SlotWriter final : public HidReportWriter {
  public:
    explicit SlotWriter(std::shared_ptr<SlotWrites> writes) : writes_(std::move(writes)) {}
    bool write(const std::uint8_t* data, std::size_t len) override {
        const std::lock_guard<std::mutex> lock(writes_->mtx);
        writes_->reports.emplace_back(data, data + len);
        return true;
    }
    FeatureWrite writeFeature(const std::uint8_t* data, std::size_t len) override {
        const std::lock_guard<std::mutex> lock(writes_->mtx);
        writes_->features.emplace_back(data, data + len);
        writes_->featureWriters.push_back(std::this_thread::get_id());
        return FeatureWrite::Taken;
    }

  private:
    std::shared_ptr<SlotWrites> writes_;
};

bool waitForFeatures(const SlotWrites& writes, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (writes.featureCount() >= count) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

constexpr std::uint8_t kSteamClearMappings = 0x81;
constexpr std::uint8_t kSteamSetSettings = 0x87;
constexpr std::uint8_t kSteamDefaultMappings = 0x85;
constexpr std::uint8_t kSteamDefaultSettings = 0x8E;

// Every claim gets an id of its own, and the gateway remembers which interface each one opened
// and the report sink it was handed, which a test plays the slot's read thread through. A slot's
// controller is there while its interface is in `connected`, which is what the real gateway
// learns from the slot's wireless status events.
class ReceiverGateway : public UsbDeviceGateway {
  public:
    std::vector<UsbDeviceInfo> devices;
    std::set<int> connected;
    std::optional<int> refusedInterface;
    std::int64_t reads = 0;
    std::vector<int> claimedInterfaces;
    std::vector<int> released;
    std::map<int, int> interfaceOf;
    std::map<int, std::function<void(const UsbReport&)>> sinkOf;
    std::map<int, std::shared_ptr<SlotWrites>> writesOf;
    int nextSyntheticId = -1000;

    std::vector<UsbDeviceInfo> enumerate() override { return devices; }
    ClaimResult claim(const UsbDeviceInfo& device,
                      std::function<void(const UsbReport&)> onReport) override {
        claimedInterfaces.push_back(device.interfaceNumber);
        if (refusedInterface == device.interfaceNumber) {
            return ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
        }
        const int syntheticId = nextSyntheticId--;
        interfaceOf[syntheticId] = device.interfaceNumber;
        sinkOf[device.interfaceNumber] = std::move(onReport);
        return ClaimResult::success(syntheticId);
    }

    // What the slot's read thread would hand up.
    void readOn(int interfaceNumber, const UsbReport& report) const {
        sinkOf.at(interfaceNumber)(report);
    }

    // Each slot's worker writes to a log of its own, by the slot's interface.
    std::unique_ptr<HidReportWriter> openReportWriter(int syntheticId) override {
        return std::make_unique<SlotWriter>(writesFor(interfaceOf.at(syntheticId)));
    }
    const SlotWrites& writes(int interfaceNumber) { return *writesFor(interfaceNumber); }

  private:
    std::shared_ptr<SlotWrites> writesFor(int interfaceNumber) {
        auto& writes = writesOf[interfaceNumber];
        if (writes == nullptr) { writes = std::make_shared<SlotWrites>(); }
        return writes;
    }

  public:
    void releaseClaim(int syntheticId) override { released.push_back(syntheticId); }
    bool isKnownFastLaneModel(int /*vendorId*/, int /*productId*/) const override { return false; }
    bool isClaimAlive(int syntheticId) const override {
        return std::find(released.begin(), released.end(), syntheticId) == released.end();
    }
    bool isPadLinked(int syntheticId) const override {
        const auto it = interfaceOf.find(syntheticId);
        return it != interfaceOf.end() && connected.count(it->second) != 0;
    }
    std::int64_t completionCount(int /*syntheticId*/) const override { return reads; }
    std::int64_t decodedCount(int /*syntheticId*/) const override { return 0; }
    bool writeOutputReport(int /*syntheticId*/, const std::uint8_t* /*data*/,
                           std::size_t /*len*/) override {
        return false;
    }

    int syntheticIdOf(int interfaceNumber) const {
        for (const auto& [syntheticId, iface] : interfaceOf) {
            if (iface == interfaceNumber) { return syntheticId; }
        }
        return 0;
    }
};

// The pads the manager put on the slot list and took off it, by synthetic id, and the battery
// readings it mirrored up, by slot.
struct PadObserver : UsbDirectObserver {
    struct Battery {
        int slot;
        std::uint8_t level;
    };
    std::vector<int> added;
    std::vector<int> removed;
    std::vector<DirectClaimFailure> failures;
    std::vector<Battery> batteries;
    int changes = 0;
    void padBatteryChanged(const DirectPadKey& pad, std::uint8_t level,
                           std::uint8_t /*status*/) override {
        batteries.push_back({pad.slot, level});
    }
    void syntheticAdded(int syntheticId, const std::string& /*name*/, bool /*hasGyro*/,
                        int /*pollRateHz*/, int /*vendorId*/, int /*productId*/) override {
        added.push_back(syntheticId);
    }
    void syntheticRemoved(int syntheticId) override { removed.push_back(syntheticId); }
    void markFailure(int /*vendorId*/, int /*productId*/, DirectClaimFailure reason) override {
        failures.push_back(reason);
    }
    void controllersChanged() override { ++changes; }
};

// A receiver with a controller on each of `slotCount` interfaces from `firstSlot` on.
struct ReceiverRig {
    ReceiverGateway gateway;
    UsbPathPreferenceRepository repo{makeSharedSettings()};
    UsbPathPreferenceStore prefs{&repo};
    PadObserver observer;
    UsbGamepadManager manager{&gateway, nullptr, &prefs, &observer};
    int productId = 0;

    ReceiverRig(int receiverProductId, int firstSlot, int slotCount)
        : productId(receiverProductId) {
        for (const int iface : slotInterfaces(firstSlot, slotCount)) {
            gateway.devices.push_back(slotDevice(productId, iface));
            gateway.connected.insert(iface);
        }
    }

    void pickDirect() {
        manager.tryDirectMode(kValve, productId);
        manager.reconcile();
    }

    // The interfaces of the pads on the slot list.
    std::vector<int> listedSlots() const {
        std::vector<int> out;
        for (const auto& pad : manager.directPads()) { out.push_back(pad.key.slot); }
        return out;
    }

    std::vector<std::string> listedSlotIds() const {
        std::vector<std::string> out;
        for (const auto& pad : manager.directPads()) {
            out.push_back(dish::reducer::directSlotId(pad.key));
        }
        return out;
    }
};

// The 2015 receiver with all four slots paired.
struct FullReceiver : ReceiverRig {
    FullReceiver() : ReceiverRig(kReceiver2015, kFirstSlot2015, 4) {}
};

} // namespace

TEST_CASE("every controller connected to a receiver reads as a pad of its own",
          "[usb-manager][receiver]") {
    const auto [productId, firstSlot] = GENERATE(table<int, int>({
        {kReceiver2015, kFirstSlot2015},
        {kProteusReceiver, kFirstSlot2026},
        {kNereidReceiver, kFirstSlot2026},
    }));
    const int slotCount = GENERATE(2, 3, 4);
    ReceiverRig rig(productId, firstSlot, slotCount);

    rig.manager.tryDirectMode(kValve, productId);
    rig.manager.reconcile();

    CHECK(rig.gateway.claimedInterfaces == slotInterfaces(firstSlot, slotCount));
    CHECK(rig.observer.added.size() == static_cast<std::size_t>(slotCount));
}

TEST_CASE("each pad of a receiver has its own synthetic id and slot id",
          "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.pickDirect();

    const auto pads = rig.manager.directPads();
    REQUIRE(pads.size() == 4U);
    std::set<int> syntheticIds;
    for (const auto& pad : pads) { syntheticIds.insert(pad.syntheticId); }
    CHECK(syntheticIds.size() == 4U);
    CHECK(rig.listedSlotIds() ==
          std::vector<std::string>{"685642050:1", "685642050:2", "685642050:3", "685642050:4"});
}

TEST_CASE("an empty receiver slot is claimed to listen, and is no pad", "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.gateway.connected = {1, 3};

    rig.pickDirect();

    CHECK(rig.gateway.claimedInterfaces == std::vector<int>{1, 2, 3, 4});
    CHECK(rig.listedSlots() == std::vector<int>{1, 3});
    CHECK(rig.observer.added.size() == 2U);
    CHECK(rig.gateway.released.empty());
}

TEST_CASE("a receiver read with no controller connected keeps its claim and lists no pad",
          "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.gateway.connected.clear();

    rig.pickDirect();

    const auto c = rig.manager.controllerFor(kValve, kReceiver2015);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Direct);
    CHECK(rig.manager.directPads().empty());
    CHECK(rig.observer.added.empty());
}

TEST_CASE("a controller that connects to a slot becomes its pad on the next scan",
          "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.gateway.connected = {1};
    rig.pickDirect();
    REQUIRE(rig.listedSlots() == std::vector<int>{1});
    const int changesBefore = rig.observer.changes;

    rig.gateway.connected.insert(4);
    rig.manager.reconcile();

    CHECK(rig.listedSlots() == std::vector<int>{1, 4});
    REQUIRE(rig.observer.added.size() == 2U);
    CHECK(rig.observer.added.back() == rig.gateway.syntheticIdOf(4));
    CHECK(rig.observer.changes > changesBefore);
}

TEST_CASE("a controller that disconnects leaves the slot list and its slot keeps listening",
          "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.pickDirect();
    REQUIRE(rig.listedSlots() == std::vector<int>{1, 2, 3, 4});

    rig.gateway.connected.erase(2);
    rig.manager.reconcile();

    CHECK(rig.listedSlots() == std::vector<int>{1, 3, 4});
    CHECK(rig.observer.removed == std::vector<int>{rig.gateway.syntheticIdOf(2)});
    CHECK(rig.gateway.released.empty());
    CHECK(rig.gateway.isClaimAlive(rig.gateway.syntheticIdOf(2)));
}

TEST_CASE("a controller that reconnects lands on its own slot id", "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.pickDirect();
    rig.gateway.connected.erase(3);
    rig.manager.reconcile();
    rig.gateway.connected.erase(1);
    rig.manager.reconcile();
    REQUIRE(rig.listedSlotIds() == std::vector<std::string>{"685642050:2", "685642050:4"});

    // Back in the other order: the interface names the slot, not the order of arrival.
    rig.gateway.connected.insert(3);
    rig.manager.reconcile();
    rig.gateway.connected.insert(1);
    rig.manager.reconcile();

    CHECK(rig.listedSlotIds() ==
          std::vector<std::string>{"685642050:1", "685642050:2", "685642050:3", "685642050:4"});
}

TEST_CASE("a receiver plugged back in gives each slot the id it had", "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.pickDirect();
    const auto before = rig.listedSlotIds();
    const auto devices = rig.gateway.devices;

    rig.gateway.devices.clear();
    rig.manager.reconcile();
    rig.manager.reconcile();
    REQUIRE_FALSE(rig.manager.controllerFor(kValve, kReceiver2015).has_value());
    REQUIRE(rig.manager.directPads().empty());

    // The system may list the interfaces in another order after a replug.
    rig.gateway.devices.assign(devices.rbegin(), devices.rend());
    rig.manager.reconcile();
    rig.manager.reconcile();

    CHECK(rig.listedSlotIds() == before);
}

TEST_CASE("one Direct pick is the receiver's, for every slot", "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.pickDirect();
    REQUIRE(rig.manager.directPads().size() == 4U);
    CHECK(rig.prefs.choiceFor(kValve, kReceiver2015) == PathChoice::Direct);

    rig.manager.setPathChoice(kValve, kReceiver2015, PathChoice::Standard);

    CHECK(rig.manager.directPads().empty());
    CHECK(rig.gateway.released.size() == 4U);
    CHECK(rig.observer.removed.size() == 4U);
    CHECK(rig.prefs.choiceFor(kValve, kReceiver2015) == PathChoice::Standard);
}

TEST_CASE("a stored Direct pick claims every slot of a receiver as it arrives",
          "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.prefs.setChoice(kValve, kReceiver2015, PathChoice::Direct);

    rig.manager.reconcile();
    rig.manager.reconcile();

    CHECK(rig.gateway.claimedInterfaces == std::vector<int>{1, 2, 3, 4});
    CHECK(rig.listedSlots() == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("a slot that will not open keeps the whole receiver on Standard",
          "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.gateway.refusedInterface = 3;

    rig.pickDirect();

    const auto c = rig.manager.controllerFor(kValve, kReceiver2015);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Routed);
    CHECK(c->failure == DirectClaimFailure::Busy);
    // Every slot that did open was let go again.
    const auto opened =
        std::count_if(rig.gateway.claimedInterfaces.begin(), rig.gateway.claimedInterfaces.end(),
                      [](int iface) { return iface != 3; });
    CHECK(rig.gateway.released.size() == static_cast<std::size_t>(opened));
    CHECK(rig.manager.directPads().empty());
    CHECK(rig.observer.added.empty());
}

TEST_CASE("feedback for a receiver's pad reaches that slot's claim", "[usb-manager][receiver]") {
    FullReceiver rig;
    rig.pickDirect();

    const auto target = rig.manager.directTarget(DirectPadKey{kValve, kReceiver2015, 3});

    REQUIRE(target.has_value());
    CHECK(target->syntheticId == rig.gateway.syntheticIdOf(3));
    CHECK(target->worker != nullptr);
    CHECK(rig.manager.isDirectClaimed(DirectPadKey{kValve, kReceiver2015, 4}));
    CHECK_FALSE(rig.manager.isDirectClaimed(DirectPadKey{kValve, kReceiver2015, 5}));
}

TEST_CASE("an empty slot that reads nothing it can decode is not judged unreadable",
          "[usb-manager][receiver][health]") {
    FullReceiver rig;
    rig.gateway.connected.clear();
    rig.gateway.reads = 50;
    rig.pickDirect();

    for (int scan = 0; scan < 5; ++scan) { rig.manager.reconcile(); }

    const auto c = rig.manager.controllerFor(kValve, kReceiver2015);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Direct);
    CHECK(rig.observer.failures.empty());
}

TEST_CASE("a connected slot whose reports never decode releases the receiver",
          "[usb-manager][receiver][health]") {
    FullReceiver rig;
    rig.gateway.reads = 50;
    rig.pickDirect();

    rig.manager.reconcile();

    const auto c = rig.manager.controllerFor(kValve, kReceiver2015);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Routed);
    CHECK(c->failure == DirectClaimFailure::InitFailed);
    CHECK(rig.gateway.released.size() == 4U);
}

TEST_CASE("a model whose pad is the device stays one pad, keyed by its model",
          "[usb-manager][receiver]") {
    ReceiverGateway gateway;
    UsbDeviceInfo wired = slotDevice(kWiredSteamController, 2);
    wired.name = "Valve Steam Controller";
    gateway.devices = {wired, wired};
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager manager(&gateway, nullptr, &prefs, nullptr);

    manager.tryDirectMode(kValve, kWiredSteamController);
    manager.reconcile();

    CHECK(gateway.claimedInterfaces.size() == 1U);
    const auto pads = manager.directPads();
    REQUIRE(pads.size() == 1U);
    CHECK(dish::reducer::directSlotId(pads.front().key) ==
          std::to_string((kValve << 16) | kWiredSteamController));
    CHECK(manager.directTarget(kValve, kWiredSteamController).has_value());
}

TEST_CASE("a controller arriving on a slot is quieted by the slot's worker, not by the reader",
          "[usb-manager][receiver][steam]") {
    FullReceiver rig;
    rig.gateway.connected.clear();
    rig.pickDirect();
    const std::thread::id reader = std::this_thread::get_id();

    rig.gateway.readOn(2, controllerArrivedEvent());

    const SlotWrites& slot2 = rig.gateway.writes(2);
    CHECK_FALSE(slot2.wroteOn(reader));
    REQUIRE(waitForFeatures(slot2, 2));
    CHECK(slot2.messages() == std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings});
    CHECK_FALSE(slot2.wroteOn(reader));
    CHECK(rig.gateway.writes(1).featureCount() == 0U);
    CHECK(rig.gateway.writes(3).featureCount() == 0U);
}

TEST_CASE("a model's own Steam Controller is quieted by its worker at the claim",
          "[usb-manager][receiver][steam]") {
    ReceiverGateway gateway;
    UsbDeviceInfo wired = slotDevice(kWiredSteamController, 0);
    wired.name = "Valve Steam Controller";
    gateway.devices = {wired};
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager manager(&gateway, nullptr, &prefs, nullptr);
    const std::thread::id caller = std::this_thread::get_id();

    manager.tryDirectMode(kValve, kWiredSteamController);
    manager.reconcile();

    const SlotWrites& pad = gateway.writes(0);
    REQUIRE(waitForFeatures(pad, 2));
    CHECK(pad.messages() == std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings});
    CHECK_FALSE(pad.wroteOn(caller));
}

TEST_CASE("a controller that comes back reports its battery again, changed or not",
          "[usb-manager][receiver][battery]") {
    // The card forgets a pad's reading when the pad leaves the slot list, so the first reading
    // after the controller returns has to reach it, even when the level is the one it had.
    FullReceiver rig;
    rig.pickDirect();

    rig.gateway.readOn(2, batteryReport(5));
    rig.gateway.readOn(2, batteryReport(5));
    REQUIRE(rig.observer.batteries.size() == 1U);
    CHECK(rig.observer.batteries.front().slot == 2);
    CHECK(rig.observer.batteries.front().level == 5);

    rig.gateway.readOn(2, controllerLeftEvent());
    rig.gateway.readOn(2, batteryReport(5));

    REQUIRE(rig.observer.batteries.size() == 2U);
    CHECK(rig.observer.batteries.back().slot == 2);
    CHECK(rig.observer.batteries.back().level == 5);
}

TEST_CASE("a controller that leaves publishes a blank input, so its last one does not stay latched",
          "[usb-manager][receiver]") {
    ReceiverGateway gateway;
    gateway.devices = {slotDevice(kReceiver2015, 1)};
    gateway.connected = {1};
    dish::input::GamepadInputProcessor processor;
    std::vector<std::uint16_t> buttons;
    processor.setReportSender(
        [&buttons](const std::string& /*id*/, std::uint16_t b, std::uint8_t /*lt*/,
                   std::uint8_t /*rt*/, std::int16_t /*lx*/, std::int16_t /*ly*/,
                   std::int16_t /*rx*/, std::int16_t /*ry*/) { buttons.push_back(b); });
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager manager(&gateway, &processor, &prefs, nullptr);
    manager.tryDirectMode(kValve, kReceiver2015);
    manager.reconcile();

    UsbReport held;
    held.wButtons = dish::input::GamepadInputProcessor::Buttons::kA;
    gateway.readOn(1, held);
    gateway.readOn(1, controllerLeftEvent());

    REQUIRE(buttons.size() == 2U);
    CHECK(buttons.front() == dish::input::GamepadInputProcessor::Buttons::kA);
    CHECK(buttons.back() == 0);
}

TEST_CASE("a receiver unplugged is let go with nothing written to it",
          "[usb-manager][receiver][steam]") {
    // The pad is gone: there is no controller to take the settings off, and a write to the dead
    // slot only waits on it.
    FullReceiver rig;
    rig.gateway.connected.clear();
    rig.pickDirect();
    rig.gateway.readOn(2, controllerArrivedEvent());
    REQUIRE(waitForFeatures(rig.gateway.writes(2), 2));

    rig.gateway.devices.clear();
    rig.manager.reconcile();
    rig.manager.reconcile();

    REQUIRE_FALSE(rig.manager.controllerFor(kValve, kReceiver2015).has_value());
    CHECK(rig.gateway.released.size() == 4U);
    CHECK(rig.gateway.writes(2).messages() ==
          std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings});
}

TEST_CASE("a Standard pick hands a connected controller its settings back, an empty slot nothing",
          "[usb-manager][receiver][steam]") {
    FullReceiver rig;
    rig.gateway.connected.clear();
    rig.pickDirect();
    rig.gateway.readOn(2, controllerArrivedEvent());
    REQUIRE(waitForFeatures(rig.gateway.writes(2), 2));

    rig.manager.setPathChoice(kValve, kReceiver2015, PathChoice::Standard);

    CHECK(rig.gateway.writes(2).messages() ==
          std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings, kSteamDefaultMappings,
                                    kSteamDefaultSettings, kSteamSetSettings});
    CHECK(rig.gateway.writes(1).featureCount() == 0U);
    CHECK(rig.gateway.writes(3).featureCount() == 0U);
}
