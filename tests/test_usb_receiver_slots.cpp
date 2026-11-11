// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A Steam Controller wireless receiver on the USB-direct path, over a fake gateway. A receiver
// serves up to four paired controllers under one vid:pid, one HID interface each: the 2015
// receiver's slots are interfaces 1 to 4 (SDL's HIDAPI_DriverSteam_IsSupportedDevice, Linux's
// steam_is_valve_interface), the 2026 receivers' 2 to 5
// (HIDAPI_DriverSteamTriton_IsSupportedDevice).

#include "source/usb/UsbGamepadManager.h"

#include "source/store/UsbPathPreferenceStore.h"
#include "source/usb/UsbDeviceGateway.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace dish::source::usb;
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

// Every claim gets an id of its own, and the gateway remembers which interface each one opened.
// A slot's controller is there while its interface is in `connected`, which is what the real
// gateway learns from the slot's wireless status events.
class ReceiverGateway : public UsbDeviceGateway {
  public:
    std::vector<UsbDeviceInfo> devices;
    std::set<int> connected;
    std::optional<int> refusedInterface;
    std::int64_t reads = 0;
    std::vector<int> claimedInterfaces;
    std::vector<int> released;
    std::map<int, int> interfaceOf;
    int nextSyntheticId = -1000;

    std::vector<UsbDeviceInfo> enumerate() override { return devices; }
    ClaimResult claim(const UsbDeviceInfo& device,
                      std::function<void(const UsbReport&)> /*onReport*/) override {
        claimedInterfaces.push_back(device.interfaceNumber);
        if (refusedInterface == device.interfaceNumber) {
            return ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
        }
        const int syntheticId = nextSyntheticId--;
        interfaceOf[syntheticId] = device.interfaceNumber;
        return ClaimResult::success(syntheticId);
    }
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

// The pads the manager put on the slot list and took off it, by synthetic id.
struct PadObserver : UsbDirectObserver {
    std::vector<int> added;
    std::vector<int> removed;
    std::vector<DirectClaimFailure> failures;
    int changes = 0;
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
