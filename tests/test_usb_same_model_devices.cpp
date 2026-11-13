// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Two devices of one model on the USB-direct path, over a fake gateway that tells them apart only
// by the path it names each one by: two identical wired pads, or two Steam Controller receivers.

#include "source/usb/UsbGamepadManager.h"

#include "source/store/UsbPathPreferenceStore.h"
#include "source/usb/UsbDeviceGateway.h"

#include "core/reducer/DirectPadKey.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace dish::source::usb;
using dish::reducer::DirectClaimFailure;
using dish::source::UsbPathPreferenceRepository;
using dish::source::UsbPathPreferenceStore;
using dish::test::makeSharedSettings;

namespace {

constexpr int kSony = 0x054C;
constexpr int kDualSense = 0x0CE6;
// 0x054C0CE6, the DualSense's model key: the slot id its one pad always had.
const std::string kDualSenseKey = "88870118";
constexpr int kValve = 0x28DE;
constexpr int kReceiver2015 = 0x1142;
constexpr int kProteusReceiver = 0x1304;
constexpr int kNereidReceiver = 0x1305;
constexpr int kFirstSlot2015 = 1;
constexpr int kFirstSlot2026 = 2;
constexpr int kSlotsPerReceiver = 4;
// The ports the devices are plugged into, the way the Linux gateway names a USB device.
const std::string kFirstPort = "1-1";
const std::string kSecondPort = "1-2";
const std::string kThirdPort = "1-3";

UsbDeviceInfo dualSenseOn(const std::string& port) {
    UsbDeviceInfo d;
    d.vendorId = kSony;
    d.productId = kDualSense;
    d.name = "DualSense Wireless Controller";
    d.devicePath = port;
    return d;
}

// A receiver with a controller on each of its slots.
std::vector<UsbDeviceInfo> receiverOn(const std::string& port, int productId, int firstSlot) {
    std::vector<UsbDeviceInfo> pads;
    for (int iface = firstSlot; iface < firstSlot + kSlotsPerReceiver; ++iface) {
        UsbDeviceInfo d;
        d.vendorId = kValve;
        d.productId = productId;
        d.name = "Valve Steam Controller (dongle)";
        d.interfaceNumber = iface;
        d.devicePath = port;
        pads.push_back(d);
    }
    return pads;
}

std::string modelKeyOf(int vendorId, int productId) {
    return std::to_string(dish::reducer::slotPathVpKey(vendorId, productId));
}

// The slot ids of a receiver's controllers, `device` being the model key and the unit, if any.
std::vector<std::string> slotIdsOf(const std::string& device, int firstSlot) {
    std::vector<std::string> ids;
    for (int iface = firstSlot; iface < firstSlot + kSlotsPerReceiver; ++iface) {
        ids.push_back(device + ":" + std::to_string(iface));
    }
    return ids;
}

// Every claim gets an id of its own and the gateway remembers what it opened, so a claim reads
// for as long as its device still enumerates and nobody let go of it, the way a real read loop
// ends with its device. Every pad decodes, so no claim is judged unreadable.
class SameModelGateway : public UsbDeviceGateway {
  public:
    std::vector<UsbDeviceInfo> devices;
    // Claims on this device fail Busy, the way a pad another program holds does.
    std::optional<std::string> refusedDevice;
    std::map<std::string, int> attemptsOn;
    std::map<int, UsbDeviceInfo> opened;
    std::vector<int> released;
    bool fastLane = false;
    int nextSyntheticId = -1000;

    std::vector<UsbDeviceInfo> enumerate() override { return devices; }
    ClaimResult claim(const UsbDeviceInfo& device,
                      std::function<void(const UsbReport&)> /*onReport*/) override {
        ++attemptsOn[device.devicePath];
        if (refusedDevice == device.devicePath) {
            return ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
        }
        const int syntheticId = nextSyntheticId--;
        opened[syntheticId] = device;
        return ClaimResult::success(syntheticId);
    }
    void releaseClaim(int syntheticId) override { released.push_back(syntheticId); }
    bool isKnownFastLaneModel(int /*vendorId*/, int /*productId*/) const override {
        return fastLane;
    }
    bool isClaimAlive(int syntheticId) const override {
        const auto it = opened.find(syntheticId);
        return it != opened.end() && !wasReleased(syntheticId) && stillEnumerates(it->second);
    }
    std::int64_t completionCount(int /*syntheticId*/) const override { return 1; }
    std::int64_t decodedCount(int /*syntheticId*/) const override { return 1; }
    bool writeOutputReport(int /*syntheticId*/, const std::uint8_t* /*data*/,
                           std::size_t /*len*/) override {
        return true;
    }

    void unplug(const std::string& port) {
        std::erase_if(devices, [&port](const UsbDeviceInfo& d) { return d.devicePath == port; });
    }

    // Every claim ever opened on the device plugged into `port`.
    std::vector<int> claimsOn(const std::string& port) const {
        std::vector<int> ids;
        for (const auto& [syntheticId, device] : opened) {
            if (device.devicePath == port) { ids.push_back(syntheticId); }
        }
        return ids;
    }

    bool wasReleased(int syntheticId) const {
        return std::find(released.begin(), released.end(), syntheticId) != released.end();
    }

  private:
    bool stillEnumerates(const UsbDeviceInfo& claimed) const {
        return std::any_of(devices.begin(), devices.end(), [&claimed](const UsbDeviceInfo& d) {
            return d.devicePath == claimed.devicePath &&
                   d.interfaceNumber == claimed.interfaceNumber;
        });
    }
};

struct SameModelRig {
    SameModelGateway gateway;
    UsbPathPreferenceRepository repo{makeSharedSettings()};
    UsbPathPreferenceStore prefs{&repo};
    UsbGamepadManager manager{&gateway, nullptr, &prefs, nullptr};

    // A receiver's controllers join the slot list on the scan after the pick.
    void pickDirect(int vendorId, int productId) {
        manager.tryDirectMode(vendorId, productId);
        manager.reconcile();
    }

    // A departed device misses two scans before the sweep lets it go.
    void scanPastTheSweep() {
        manager.reconcile();
        manager.reconcile();
    }

    std::vector<std::string> listedSlotIds() const {
        std::vector<std::string> ids;
        for (const auto& pad : manager.directPads()) {
            ids.push_back(dish::reducer::directSlotId(pad.key));
        }
        return ids;
    }

    // The synthetic id the slot list carries the pad under, or 0 when it lists no such pad.
    int listedClaimOf(const std::string& slotId) const {
        for (const auto& pad : manager.directPads()) {
            if (dish::reducer::directSlotId(pad.key) == slotId) { return pad.syntheticId; }
        }
        return 0;
    }
};

// Two DualSense pads, each on a port of its own.
struct TwoDualSenses : SameModelRig {
    TwoDualSenses() { gateway.devices = {dualSenseOn(kFirstPort), dualSenseOn(kSecondPort)}; }
};

} // namespace

TEST_CASE("two pads of one model on separate devices are claimed as two pads",
          "[usb-manager][same-model]") {
    TwoDualSenses rig;

    rig.pickDirect(kSony, kDualSense);

    CHECK(rig.gateway.claimsOn(kFirstPort).size() == 1U);
    CHECK(rig.gateway.claimsOn(kSecondPort).size() == 1U);
    CHECK(rig.listedSlotIds() == std::vector<std::string>{kDualSenseKey, kDualSenseKey + "#1"});
}

TEST_CASE("whichever of two identical pads is unplugged, the other keeps its claim and its slot",
          "[usb-manager][same-model]") {
    const auto [leaving, staying, stayingSlotId] =
        GENERATE(table<std::string, std::string, std::string>({
            {kFirstPort, kSecondPort, kDualSenseKey + "#1"},
            {kSecondPort, kFirstPort, kDualSenseKey},
        }));
    TwoDualSenses rig;
    rig.pickDirect(kSony, kDualSense);
    REQUIRE(rig.listedSlotIds().size() == 2U);
    const int stayingClaim = rig.listedClaimOf(stayingSlotId);

    rig.gateway.unplug(leaving);
    rig.scanPastTheSweep();

    CHECK(rig.listedSlotIds() == std::vector<std::string>{stayingSlotId});
    CHECK(rig.listedClaimOf(stayingSlotId) == stayingClaim);
    CHECK(rig.gateway.claimsOn(staying) == std::vector<int>{stayingClaim});
    CHECK_FALSE(rig.gateway.wasReleased(stayingClaim));
}

TEST_CASE("each pad keeps its claim scan after scan, and a pad plugged in later takes the first "
          "free slot id",
          "[usb-manager][same-model]") {
    TwoDualSenses rig;
    rig.pickDirect(kSony, kDualSense);
    rig.scanPastTheSweep();
    REQUIRE(rig.gateway.opened.size() == 2U);

    rig.gateway.unplug(kFirstPort);
    rig.scanPastTheSweep();
    rig.gateway.devices.push_back(dualSenseOn(kThirdPort));
    rig.manager.reconcile();

    CHECK(rig.listedSlotIds() == std::vector<std::string>{kDualSenseKey, kDualSenseKey + "#1"});
    CHECK(rig.listedClaimOf(kDualSenseKey) == rig.gateway.claimsOn(kThirdPort).at(0));
    CHECK(rig.gateway.claimsOn(kSecondPort).size() == 1U);
}

TEST_CASE("on Auto, the pad that failed to claim stays off Direct and its twin stays on it",
          "[usb-manager][same-model]") {
    TwoDualSenses rig;
    rig.gateway.fastLane = true;
    rig.gateway.refusedDevice = kFirstPort;
    rig.pickDirect(kSony, kDualSense);
    REQUIRE(rig.listedSlotIds() == std::vector<std::string>{kDualSenseKey + "#1"});
    const int attemptsBefore = rig.gateway.attemptsOn[kFirstPort];

    rig.manager.clearChoice(kSony, kDualSense);

    CHECK(rig.listedSlotIds() == std::vector<std::string>{kDualSenseKey + "#1"});
    CHECK(rig.gateway.attemptsOn[kFirstPort] == attemptsBefore);
    CHECK(rig.gateway.claimsOn(kSecondPort).size() == 1U);
}

TEST_CASE("two receivers of one model each read their own controllers as pads",
          "[usb-manager][same-model][receiver]") {
    const auto [productId, firstSlot] = GENERATE(table<int, int>({
        {kReceiver2015, kFirstSlot2015},
        {kProteusReceiver, kFirstSlot2026},
        {kNereidReceiver, kFirstSlot2026},
    }));
    SameModelRig rig;
    rig.gateway.devices = receiverOn(kFirstPort, productId, firstSlot);
    for (const auto& slot : receiverOn(kSecondPort, productId, firstSlot)) {
        rig.gateway.devices.push_back(slot);
    }

    rig.pickDirect(kValve, productId);

    CHECK(rig.gateway.claimsOn(kFirstPort).size() == std::size_t{kSlotsPerReceiver});
    CHECK(rig.gateway.claimsOn(kSecondPort).size() == std::size_t{kSlotsPerReceiver});
    std::vector<std::string> expected = slotIdsOf(modelKeyOf(kValve, productId), firstSlot);
    for (const auto& id : slotIdsOf(modelKeyOf(kValve, productId) + "#1", firstSlot)) {
        expected.push_back(id);
    }
    CHECK(rig.listedSlotIds() == expected);
}

TEST_CASE("whichever of two receivers is unplugged, the other's controllers stay on their slots",
          "[usb-manager][same-model][receiver]") {
    const auto [leaving, staying, stayingDevice] =
        GENERATE(table<std::string, std::string, std::string>({
            {kFirstPort, kSecondPort, "685642050#1"},
            {kSecondPort, kFirstPort, "685642050"},
        }));
    SameModelRig rig;
    rig.gateway.devices = receiverOn(kFirstPort, kReceiver2015, kFirstSlot2015);
    for (const auto& slot : receiverOn(kSecondPort, kReceiver2015, kFirstSlot2015)) {
        rig.gateway.devices.push_back(slot);
    }
    rig.pickDirect(kValve, kReceiver2015);
    REQUIRE(rig.listedSlotIds().size() == 2U * kSlotsPerReceiver);

    rig.gateway.unplug(leaving);
    rig.scanPastTheSweep();

    CHECK(rig.listedSlotIds() == slotIdsOf(stayingDevice, kFirstSlot2015));
    CHECK(rig.gateway.claimsOn(staying).size() == std::size_t{kSlotsPerReceiver});
    for (const int claim : rig.gateway.claimsOn(staying)) {
        CHECK_FALSE(rig.gateway.wasReleased(claim));
    }
}

TEST_CASE("a claim opens only a node of the device it was asked for", "[usb-gateway][same-model]") {
    const UsbDeviceInfo second = dualSenseOn(kSecondPort);
    CHECK_FALSE(isTheClaimedPad(kFirstPort, 0, second));
    CHECK(isTheClaimedPad(kSecondPort, 0, second));

    const UsbDeviceInfo slot = receiverOn(kSecondPort, kReceiver2015, kFirstSlot2015).at(2);
    CHECK_FALSE(isTheClaimedPad(kFirstPort, slot.interfaceNumber, slot));
    CHECK_FALSE(isTheClaimedPad(kSecondPort, slot.interfaceNumber + 1, slot));
    CHECK(isTheClaimedPad(kSecondPort, slot.interfaceNumber, slot));
}
