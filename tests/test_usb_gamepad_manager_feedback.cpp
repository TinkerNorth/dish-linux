// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The OUT direction of the raw-HID claim driver, over a fake gateway that
// records every report instead of writing it.
//
// The builders' bytes are pinned in test_usb_output_reports.cpp. What this file
// pins is the part only the manager can get wrong: aiming a report at the right
// synthetic id, refusing to build one for a pad that is not claimed, keeping the
// DualSense's one-time lightbar handoff per claim rather than per process, and
// advancing the Switch Pro's packet counter across calls.

#include "source/usb/UsbGamepadManager.h"

#include "source/usb/UsbDeviceGateway.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

using namespace dish::source::usb;
using dish::input::usbout::kTriggerEffectBlockBytes;

namespace {

// A real DualSense identity, so parserForDevice resolves to the one family with
// every actuator. The manager derives the family from (vid, pid) alone, exactly
// as the capability solve did when it built the descriptor.
constexpr int kSonyVid = 0x054C;
constexpr int kDualSensePid = 0x0CE6;
constexpr int kDualShock4Pid = 0x09CC;
constexpr int kNintendoVid = 0x057E;
constexpr int kSwitchProPid = 0x2009;
// A generic HID pad: claimable, but its family has no actuator this path can
// drive. An Xbox pad cannot stand in here the way it does on Windows, because
// xpad publishes no hidraw node for one and it never reaches this gateway.
constexpr int kGenericVid = 0x0E6F;
constexpr int kGenericPid = 0x0180;

UsbDeviceInfo device(int vid, int pid) {
    UsbDeviceInfo d;
    d.vendorId = vid;
    d.productId = pid;
    d.name = "Pad";
    d.endpointInMaxPacket = 64;
    d.endpointInInterval = 1;
    d.hasOutEndpoint = true;
    return d;
}

class RecordingGateway : public UsbDeviceGateway {
  public:
    std::vector<UsbDeviceInfo> devices;
    int nextSyntheticId = -1000;
    bool writeSucceeds = true;

    struct Written {
        int syntheticId = 0;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Written> writes;
    std::vector<int> released;

    std::vector<UsbDeviceInfo> enumerate() override { return devices; }
    ClaimResult claim(const UsbDeviceInfo& /*d*/,
                      std::function<void(const UsbReport&)> onReport) override {
        // Kept so a test can play the read thread and feed reports back in.
        lastOnReport = std::move(onReport);
        return ClaimResult::success(nextSyntheticId--);
    }
    void releaseClaim(int syntheticId) override { released.push_back(syntheticId); }
    bool isKnownFastLaneModel(int /*v*/, int /*p*/) const override { return true; }
    std::int64_t completionCount(int /*syntheticId*/) const override { return 0; }
    bool writeOutputReport(int syntheticId, const std::uint8_t* data, std::size_t len) override {
        writes.push_back(Written{syntheticId, std::vector<std::uint8_t>(data, data + len)});
        return writeSucceeds;
    }
    bool setPadMicMuted(int syntheticId, bool muted) override {
        muteWrites.push_back({syntheticId, muted});
        return true;
    }

    std::function<void(const UsbReport&)> lastOnReport;
    std::vector<std::pair<int, bool>> muteWrites;
};

// A manager with one claimed pad of the given identity. Claims are driven the
// way production does: enumerate, then reconcile onto the fast lane.
struct Harness {
    RecordingGateway gateway;
    std::unique_ptr<UsbGamepadManager> manager;

    explicit Harness(int vid, int pid) {
        gateway.devices.push_back(device(vid, pid));
        manager = std::make_unique<UsbGamepadManager>(&gateway, nullptr, nullptr, nullptr);
        manager->reconcile();
    }
};

} // namespace

TEST_CASE("a claimed pad's feedback reaches the gateway with its synthetic id",
          "[usb][manager][feedback]") {
    Harness h(kSonyVid, kDualSensePid);
    REQUIRE(h.manager->isDirectClaimed(kSonyVid, kDualSensePid));
    const auto claimed = h.manager->controllerFor(kSonyVid, kDualSensePid);
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->syntheticId.has_value());

    REQUIRE(h.manager->applyRumble(kSonyVid, kDualSensePid, 0x8000, 0x4000));
    REQUIRE(h.gateway.writes.size() == 1U);
    CHECK(h.gateway.writes[0].syntheticId == *claimed->syntheticId);
    // Report 0x02 with COMPATIBLE_VIBRATION and the two motors: the DualSense
    // rumble report, aimed at the claim the FSM actually holds.
    REQUIRE(h.gateway.writes[0].bytes.size() == 63U);
    CHECK(h.gateway.writes[0].bytes[0] == 0x02);
    CHECK(h.gateway.writes[0].bytes[3] == 0x40);
    CHECK(h.gateway.writes[0].bytes[4] == 0x80);
}

TEST_CASE("an unclaimed model writes nothing", "[usb][manager][feedback]") {
    // The normal case for a pad the user left on the Standard path: the
    // descriptor never advertised the actuator, so a stray message must be a
    // silent no-op rather than a write to somebody else's claim.
    Harness h(kSonyVid, kDualSensePid);
    CHECK_FALSE(h.manager->isDirectClaimed(kSonyVid, kDualShock4Pid));
    CHECK_FALSE(h.manager->applyRumble(kSonyVid, kDualShock4Pid, 1, 1));
    CHECK_FALSE(h.manager->applyLightbar(kSonyVid, kDualShock4Pid, 1, 2, 3));
    CHECK_FALSE(h.manager->applyPlayerLeds(kSonyVid, kDualShock4Pid, 1));
    CHECK(h.gateway.writes.empty());
}

TEST_CASE("a family without the actuator writes nothing", "[usb][manager][feedback]") {
    // A claimed DualShock 4 has a lightbar but no adaptive triggers and no
    // player LEDs. The builder returns 0 and the manager must not hand the
    // gateway an empty buffer.
    Harness h(kSonyVid, kDualShock4Pid);
    REQUIRE(h.manager->isDirectClaimed(kSonyVid, kDualShock4Pid));
    const std::uint8_t block[kTriggerEffectBlockBytes] = {};
    CHECK_FALSE(h.manager->applyTriggerEffects(kSonyVid, kDualShock4Pid, block, block));
    CHECK_FALSE(h.manager->applyPlayerLeds(kSonyVid, kDualShock4Pid, 0x01));
    CHECK(h.gateway.writes.empty());
    // ...but its lightbar does land.
    CHECK(h.manager->applyLightbar(kSonyVid, kDualShock4Pid, 9, 8, 7));
    REQUIRE(h.gateway.writes.size() == 1U);
    CHECK(h.gateway.writes[0].bytes[0] == 0x05);
}

TEST_CASE("a claimed pad with no drivable family writes nothing", "[usb][manager][feedback]") {
    // Kept as a case of its own because it is the one where the claim IS live:
    // the refusal has to come from the family, not from the claim lookup.
    Harness h(kGenericVid, kGenericPid);
    if (h.manager->isDirectClaimed(kGenericVid, kGenericPid)) {
        CHECK_FALSE(h.manager->applyRumble(kGenericVid, kGenericPid, 0xFFFF, 0xFFFF));
        CHECK_FALSE(h.manager->applyLightbar(kGenericVid, kGenericPid, 1, 2, 3));
        CHECK(h.gateway.writes.empty());
    }
}

TEST_CASE("the DualSense lightbar handoff rides only the first colour",
          "[usb][manager][feedback]") {
    // The state is the manager's, not the builder's, so this is where a
    // per-process flag would show up as a handoff that never repeats.
    Harness h(kSonyVid, kDualSensePid);
    REQUIRE(h.manager->applyLightbar(kSonyVid, kDualSensePid, 1, 2, 3));
    REQUIRE(h.manager->applyLightbar(kSonyVid, kDualSensePid, 4, 5, 6));
    REQUIRE(h.gateway.writes.size() == 2U);
    CHECK(h.gateway.writes[0].bytes[39] == 0x02); // setup flag on the first
    CHECK(h.gateway.writes[0].bytes[42] == 0x02);
    CHECK(h.gateway.writes[1].bytes[39] == 0x00); // and not on the second
    CHECK(h.gateway.writes[1].bytes[42] == 0x00);
    CHECK(h.gateway.writes[1].bytes[45] == 4);
}

TEST_CASE("a released claim forgets the handoff so a replug re-sends it",
          "[usb][manager][feedback]") {
    // A replugged pad has forgotten the handoff, so keeping the flag would leave
    // its lightbar stuck on the firmware's own blue for the rest of the session.
    Harness h(kSonyVid, kDualSensePid);
    REQUIRE(h.manager->applyLightbar(kSonyVid, kDualSensePid, 1, 2, 3));
    REQUIRE(h.gateway.writes[0].bytes[39] == 0x02);

    // Unplug (two missed scans is the debounce), then plug back in.
    h.gateway.devices.clear();
    h.manager->reconcile();
    h.manager->reconcile();
    h.manager->reconcile();
    h.gateway.devices.push_back(device(kSonyVid, kDualSensePid));
    h.manager->reconcile();
    REQUIRE(h.manager->isDirectClaimed(kSonyVid, kDualSensePid));

    h.gateway.writes.clear();
    REQUIRE(h.manager->applyLightbar(kSonyVid, kDualSensePid, 7, 7, 7));
    REQUIRE(h.gateway.writes.size() == 1U);
    CHECK(h.gateway.writes[0].bytes[39] == 0x02);
    CHECK(h.gateway.writes[0].bytes[42] == 0x02);
}

TEST_CASE("trigger effect blocks reach the gateway in report order", "[usb][manager][feedback]") {
    Harness h(kSonyVid, kDualSensePid);
    std::uint8_t left[kTriggerEffectBlockBytes];
    std::uint8_t right[kTriggerEffectBlockBytes];
    for (std::size_t i = 0; i < kTriggerEffectBlockBytes; ++i) {
        left[i] = static_cast<std::uint8_t>(0x10 + i);
        right[i] = static_cast<std::uint8_t>(0x20 + i);
    }
    REQUIRE(h.manager->applyTriggerEffects(kSonyVid, kDualSensePid, left, right));
    REQUIRE(h.gateway.writes.size() == 1U);
    const auto& bytes = h.gateway.writes[0].bytes;
    REQUIRE(bytes.size() == 63U);
    CHECK(bytes[1] == 0x0C);
    CHECK(bytes[11] == 0x20); // right first
    CHECK(bytes[22] == 0x10); // then left
}

TEST_CASE("the Switch Pro's packet counter advances across calls", "[usb][manager][feedback]") {
    // The pad ignores a packet whose low nibble did not move, so a fixed counter
    // means every rumble after the first is silently dropped by firmware.
    Harness h(kNintendoVid, kSwitchProPid);
    REQUIRE(h.manager->isDirectClaimed(kNintendoVid, kSwitchProPid));
    REQUIRE(h.manager->applyRumble(kNintendoVid, kSwitchProPid, 0x8000, 0x8000));
    REQUIRE(h.manager->applyRumble(kNintendoVid, kSwitchProPid, 0x8000, 0x8000));
    REQUIRE(h.gateway.writes.size() == 2U);
    CHECK(h.gateway.writes[0].bytes[1] != h.gateway.writes[1].bytes[1]);
}

TEST_CASE("the counter is shared across feedback kinds", "[usb][manager][feedback]") {
    // Rumble and the player lights ride different reports but the same counter,
    // which is what the pad expects: it sequences the CONNECTION, not a stream.
    Harness h(kNintendoVid, kSwitchProPid);
    REQUIRE(h.manager->applyRumble(kNintendoVid, kSwitchProPid, 0x8000, 0));
    REQUIRE(h.manager->applyPlayerLeds(kNintendoVid, kSwitchProPid, 0x01));
    REQUIRE(h.gateway.writes.size() == 2U);
    CHECK(h.gateway.writes[0].bytes[0] == 0x10);
    CHECK(h.gateway.writes[1].bytes[0] == 0x01);
    CHECK(h.gateway.writes[0].bytes[1] != h.gateway.writes[1].bytes[1]);
}

TEST_CASE("a failed write is reported, not swallowed", "[usb][manager][feedback]") {
    // The caller uses the return to tell "no such actuator" from "the actuator
    // was there and the write failed", which is the difference between a
    // mis-declared capability and a pad that went away mid-flight.
    Harness h(kSonyVid, kDualSensePid);
    h.gateway.writeSucceeds = false;
    CHECK_FALSE(h.manager->applyRumble(kSonyVid, kDualSensePid, 1, 1));
    CHECK(h.gateway.writes.size() == 1U); // it was attempted
}

TEST_CASE("player LEDs are masked before they reach the gateway", "[usb][manager][feedback]") {
    Harness h(kSonyVid, kDualSensePid);
    REQUIRE(h.manager->applyPlayerLeds(kSonyVid, kDualSensePid, 0xFF));
    REQUIRE(h.gateway.writes.size() == 1U);
    CHECK(h.gateway.writes[0].bytes[44] == 0x1F);
}

// ---- The mic-mute lamp and the mute latch ------------------------------------

TEST_CASE("the mute lamp reaches the claimed pad and shadows into later writes",
          "[usb][manager][feedback]") {
    Harness h(kSonyVid, kDualSensePid);
    const auto claimed = h.manager->controllerFor(kSonyVid, kDualSensePid);
    REQUIRE(claimed.has_value());

    REQUIRE(h.manager->applyMicMuteLed(kSonyVid, kDualSensePid, 1));
    REQUIRE(h.gateway.writes.size() == 1U);
    CHECK(h.gateway.writes[0].syntheticId == *claimed->syntheticId);
    REQUIRE(h.gateway.writes[0].bytes.size() == 63U);
    CHECK(h.gateway.writes[0].bytes[0] == 0x02);
    CHECK(h.gateway.writes[0].bytes[2] == 0x03);
    CHECK(h.gateway.writes[0].bytes[9] == 1);
    CHECK(h.gateway.writes[0].bytes[10] == 0x10);

    // The shadow lives in the manager's per-claim FeedbackState: a colour
    // written afterwards must carry the lamp along.
    REQUIRE(h.manager->applyLightbar(kSonyVid, kDualSensePid, 0x11, 0x22, 0x33));
    REQUIRE(h.gateway.writes.size() == 2U);
    CHECK(h.gateway.writes[1].bytes[9] == 1);
    CHECK(h.gateway.writes[1].bytes[10] == 0x10);
    CHECK(h.gateway.writes[1].bytes[45] == 0x11);
}

TEST_CASE("the lamp refuses an unclaimed model, a lamp-less family and a junk state",
          "[usb][manager][feedback]") {
    Harness ds4(kSonyVid, kDualShock4Pid);
    CHECK_FALSE(ds4.manager->applyMicMuteLed(kSonyVid, kDualShock4Pid, 1)); // no lamp
    CHECK_FALSE(ds4.manager->applyMicMuteLed(kSonyVid, kDualSensePid, 1));  // not claimed
    CHECK(ds4.gateway.writes.empty());

    Harness ds5(kSonyVid, kDualSensePid);
    CHECK_FALSE(ds5.manager->applyMicMuteLed(kSonyVid, kDualSensePid, 3)); // junk state
    CHECK(ds5.gateway.writes.empty());
}

TEST_CASE("a released claim forgets the lamp shadow", "[usb][manager][feedback]") {
    // A replugged pad must not inherit a stale lamp any more than a stale
    // lightbar handoff: the FeedbackState is per claim.
    Harness h(kSonyVid, kDualSensePid);
    REQUIRE(h.manager->applyMicMuteLed(kSonyVid, kDualSensePid, 1));
    h.manager->onUsbGone(kSonyVid, kDualSensePid);
    h.gateway.devices.clear();
    h.gateway.devices.push_back(device(kSonyVid, kDualSensePid));
    h.manager->reconcile();
    REQUIRE(h.manager->isDirectClaimed(kSonyVid, kDualSensePid));

    h.gateway.writes.clear();
    REQUIRE(h.manager->applyLightbar(kSonyVid, kDualSensePid, 1, 2, 3));
    REQUIRE(h.gateway.writes.size() == 1U);
    CHECK(h.gateway.writes[0].bytes[9] == 0); // no resurrected lamp
    CHECK((h.gateway.writes[0].bytes[2] & 0x03) == 0);
}

TEST_CASE("setPadMicMuted plumbs to the claimed pad's latch and nowhere else",
          "[usb][manager][feedback]") {
    Harness h(kSonyVid, kDualSensePid);
    const auto claimed = h.manager->controllerFor(kSonyVid, kDualSensePid);
    REQUIRE(h.manager->setPadMicMuted(kSonyVid, kDualSensePid, true));
    REQUIRE(h.gateway.muteWrites.size() == 1U);
    CHECK(h.gateway.muteWrites[0].first == *claimed->syntheticId);
    CHECK(h.gateway.muteWrites[0].second);
    CHECK_FALSE(h.manager->setPadMicMuted(kSonyVid, kDualShock4Pid, true)); // not claimed
    CHECK(h.gateway.muteWrites.size() == 1U);
}

namespace {

class MuteObserver : public UsbDirectObserver {
  public:
    std::vector<std::tuple<int, int, bool>> edges;
    void padMicMuteChanged(int vendorId, int productId, bool muted) override {
        edges.emplace_back(vendorId, productId, muted);
    }
};

} // namespace

TEST_CASE("the pad's mute latch reaches the observer on the edge only",
          "[usb][manager][feedback]") {
    // The decoder owns the latch and folds the wire bit itself; the observer
    // exists so the app's store and lamp can mirror it, and it must fire once
    // per CHANGE, not once per report -- fifty extra upcalls a second would be
    // the read thread paying for nothing.
    RecordingGateway gateway;
    MuteObserver observer;
    gateway.devices.push_back(device(kSonyVid, kDualSensePid));
    UsbGamepadManager manager(&gateway, nullptr, nullptr, &observer);
    manager.reconcile();
    REQUIRE(gateway.lastOnReport);

    UsbReport report{};
    gateway.lastOnReport(report); // unmuted, matches the initial state: no edge
    CHECK(observer.edges.empty());

    report.micMuted = true;
    gateway.lastOnReport(report);
    gateway.lastOnReport(report); // held state repeats: still one edge
    REQUIRE(observer.edges.size() == 1U);
    CHECK(std::get<0>(observer.edges[0]) == kSonyVid);
    CHECK(std::get<1>(observer.edges[0]) == kDualSensePid);
    CHECK(std::get<2>(observer.edges[0]));

    report.micMuted = false;
    gateway.lastOnReport(report);
    REQUIRE(observer.edges.size() == 2U);
    CHECK_FALSE(std::get<2>(observer.edges[1]));
}
