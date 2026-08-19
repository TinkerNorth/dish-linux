// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The raw-HID claim driver over a fake gateway (no USB/HID IO). There is no
// device-detach broadcast, so an unplug is reconcile()'s presence diff, debounced
// to 2 consecutive misses so one flaky scan cannot tear down a live claim.

#include "source/usb/UsbGamepadManager.h"

#include "source/store/UsbPathPreferenceStore.h"
#include "source/usb/UsbDeviceGateway.h"

#include "Input/GamepadInputProcessor.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

using namespace dish::source::usb;
using dish::reducer::DirectClaimFailure;
using dish::reducer::PathChoice;
using dish::reducer::UsbPhase;
using dish::source::UsbPathPreferenceRepository;
using dish::source::UsbPathPreferenceStore;
using dish::test::makeSharedSettings;

namespace {

constexpr int kVid = 0x045E, kPid = 0x028E;

UsbDeviceInfo padDevice() {
    UsbDeviceInfo d;
    d.vendorId = kVid;
    d.productId = kPid;
    d.name = "Pad";
    d.endpointInMaxPacket = 64;
    d.endpointInInterval = 1;
    return d;
}

class FakeGateway : public UsbDeviceGateway {
  public:
    std::vector<UsbDeviceInfo> devices{padDevice()};
    std::optional<ClaimResult> nextClaim; // nullopt -> a default Busy.
    bool fastLane = false;
    int claimCalls = 0;
    int nextSyntheticId = -1000;
    std::vector<int> released;

    std::vector<UsbDeviceInfo> enumerate() override { return devices; }
    ClaimResult claim(const UsbDeviceInfo& /*d*/,
                      std::function<void(const UsbReport&)> /*onReport*/) override {
        ++claimCalls;
        if (nextClaim.has_value()) { return *nextClaim; }
        return ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    }
    void releaseClaim(int syntheticId) override { released.push_back(syntheticId); }
    bool isKnownFastLaneModel(int /*vendorId*/, int /*productId*/) const override {
        return fastLane;
    }
    std::int64_t completionCount(int /*syntheticId*/) const override { return 0; }
};

struct RecordingObserver : UsbDirectObserver {
    std::vector<DirectClaimFailure> failures;
    std::vector<int> syntheticsAdded;
    std::vector<int> syntheticsRemoved;
    int controllersChangedCount = 0;
    void markFailure(int /*v*/, int /*p*/, DirectClaimFailure reason) override {
        failures.push_back(reason);
    }
    void syntheticAdded(int syntheticId, const std::string& /*name*/, bool /*hasGyro*/,
                        int /*pollRateHz*/, int /*v*/, int /*p*/) override {
        syntheticsAdded.push_back(syntheticId);
    }
    void syntheticRemoved(int syntheticId) override { syntheticsRemoved.push_back(syntheticId); }
    void controllersChanged() override { ++controllersChangedCount; }
};

bool sawFailure(const RecordingObserver& obs, DirectClaimFailure reason) {
    for (auto f : obs.failures) {
        if (f == reason) { return true; }
    }
    return false;
}

} // namespace

TEST_CASE("open rejected reports Busy and drops back to Standard", "[usb-manager]") {
    FakeGateway gw;
    gw.nextClaim = ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.tryDirectMode(kVid, kPid);

    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Routed);
    CHECK(c->failure == DirectClaimFailure::Busy);
    CHECK(sawFailure(obs, DirectClaimFailure::Busy));
}

TEST_CASE("permission denied reports PermissionDenied", "[usb-manager]") {
    FakeGateway gw;
    gw.nextClaim =
        ClaimResult::fail(DirectClaimFailure::PermissionDenied, /*frameworkStolen=*/false);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.tryDirectMode(kVid, kPid);

    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Routed);
    CHECK(c->failure == DirectClaimFailure::PermissionDenied);
    CHECK(sawFailure(obs, DirectClaimFailure::PermissionDenied));
}

TEST_CASE("claim interface rejected reports Busy", "[usb-manager]") {
    FakeGateway gw;
    gw.nextClaim = ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, nullptr, &prefs, nullptr);

    m.tryDirectMode(kVid, kPid);

    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->failure == DirectClaimFailure::Busy);
    CHECK(c->phase == UsbPhase::Routed);
}

TEST_CASE("read-loop bring-up failure after claim waits for the framework as InitFailed",
          "[usb-manager]") {
    FakeGateway gw;
    gw.nextClaim = ClaimResult::fail(DirectClaimFailure::InitFailed, /*frameworkStolen=*/true);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, nullptr, &prefs, nullptr);

    m.tryDirectMode(kVid, kPid);

    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::AwaitingFramework);
    CHECK(c->failure == DirectClaimFailure::InitFailed);
}

TEST_CASE("successful claim reaches Direct and registers a synthetic", "[usb-manager]") {
    FakeGateway gw;
    gw.nextClaim = ClaimResult::success(-1000);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.tryDirectMode(kVid, kPid);

    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Direct);
    CHECK(c->syntheticId == -1000);
    CHECK_FALSE(c->failure.has_value());
    REQUIRE(obs.syntheticsAdded.size() == 1);
    CHECK(obs.syntheticsAdded.front() == -1000);
}

TEST_CASE("controllersChanged fires on a held-synthetic transition (Direct->AwaitingFramework)",
          "[usb-manager]") {
    // A Direct->Standard pick parks the controller in AwaitingFramework but KEEPS
    // the synthetic, so no granular callback fires. controllersChanged is the only
    // signal that can rebuild the slot list and run the AwaitingFramework settle.
    FakeGateway gw;
    gw.nextClaim = ClaimResult::success(-1000);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.tryDirectMode(kVid, kPid);
    {
        const auto c = m.controllerFor(kVid, kPid);
        REQUIRE(c.has_value());
        REQUIRE(c->phase == UsbPhase::Direct);
    }
    const int afterDirect = obs.controllersChangedCount;
    REQUIRE(afterDirect > 0);

    m.setPathChoice(kVid, kPid, PathChoice::Standard);
    {
        const auto c = m.controllerFor(kVid, kPid);
        REQUIRE(c.has_value());
        CHECK(c->phase == UsbPhase::AwaitingFramework); // synthetic held, not removed
    }
    CHECK(obs.controllersChangedCount > afterDirect);
}

TEST_CASE("a recorded failure suppresses auto-Direct on a verified model", "[usb-manager]") {
    FakeGateway gw;
    gw.fastLane = true;
    gw.nextClaim = ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, nullptr, &prefs, nullptr);

    m.reconcile(); // verified model -> auto-Direct -> claim runs and fails.
    REQUIRE(gw.claimCalls == 1);
    {
        const auto c = m.controllerFor(kVid, kPid);
        REQUIRE(c.has_value());
        CHECK(c->desired == PathChoice::Standard); // rolled back after the failure.
    }

    m.reconcile();
    CHECK(gw.claimCalls == 1);
    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->desired == PathChoice::Standard);
}

TEST_CASE("a verified model with no recorded failure auto-claims Direct", "[usb-manager]") {
    FakeGateway gw;
    gw.fastLane = true;
    gw.nextClaim = ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, nullptr, &prefs, nullptr);

    m.reconcile();

    CHECK(gw.claimCalls == 1);
}

TEST_CASE("unplugging a Direct pad erases it and releases the claim", "[usb-manager]") {
    FakeGateway gw;
    gw.nextClaim = ClaimResult::success(-1000);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.tryDirectMode(kVid, kPid);
    {
        const auto c = m.controllerFor(kVid, kPid);
        REQUIRE(c.has_value());
        REQUIRE(c->phase == UsbPhase::Direct);
    }

    gw.devices.clear(); // physical unplug: the pad stops enumerating.
    m.reconcile();      // miss #1: debounced.
    REQUIRE(m.controllerFor(kVid, kPid).has_value());
    CHECK(gw.released.empty());
    const int beforeSweep = obs.controllersChangedCount;

    m.reconcile(); // miss #2: swept.
    CHECK_FALSE(m.controllerFor(kVid, kPid).has_value());
    REQUIRE(gw.released.size() == 1);
    CHECK(gw.released.front() == -1000);
    REQUIRE(obs.syntheticsRemoved.size() == 1);
    CHECK(obs.syntheticsRemoved.front() == -1000);
    CHECK(obs.controllersChangedCount > beforeSweep);

    // The sweep is edge-triggered by presence: an already-empty bus stays quiet.
    const int afterSweep = obs.controllersChangedCount;
    m.reconcile();
    CHECK(obs.controllersChangedCount == afterSweep);
    CHECK(gw.released.size() == 1);
}

TEST_CASE("one missed scan is a blip, not an unplug", "[usb-manager]") {
    // A flaky enumeration pass (Bluetooth link parking, a transient exclusive
    // open elsewhere) would otherwise release and re-claim a working pad in a
    // visible loop.
    FakeGateway gw;
    gw.nextClaim = ClaimResult::success(-1000);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.tryDirectMode(kVid, kPid);
    REQUIRE(m.controllerFor(kVid, kPid).has_value());

    const auto pad = padDevice();
    gw.devices.clear();
    m.reconcile(); // miss #1
    gw.devices = {pad};
    m.reconcile(); // seen again: counter resets.
    gw.devices.clear();
    m.reconcile(); // miss #1 again (NOT #2).

    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Direct);
    CHECK(gw.released.empty());
    CHECK(obs.syntheticsRemoved.empty());
}

TEST_CASE("unplugging a Routed pad forgets it without a release", "[usb-manager]") {
    FakeGateway gw; // fastLane=false: reconcile tracks the pad Routed, no claim.
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    RecordingObserver obs;
    UsbGamepadManager m(&gw, nullptr, &prefs, &obs);

    m.reconcile();
    REQUIRE(m.controllerFor(kVid, kPid).has_value());
    REQUIRE(gw.claimCalls == 0);

    gw.devices.clear();
    m.reconcile(); // miss #1: debounced.
    m.reconcile(); // miss #2: swept.

    CHECK_FALSE(m.controllerFor(kVid, kPid).has_value());
    CHECK(gw.released.empty());
    CHECK(obs.syntheticsRemoved.empty());
}

TEST_CASE("a replug after the sweep re-tracks fresh and re-evaluates auto-Direct",
          "[usb-manager]") {
    // onUsbGone clears the recorded prior failure, so a fresh plug-in must not
    // inherit a failure from before the unplug. clearChoice stands in for the
    // user putting the model back on Auto after the failed claim stored Standard.
    FakeGateway gw;
    gw.fastLane = true;
    gw.nextClaim = ClaimResult::fail(DirectClaimFailure::Busy, /*frameworkStolen=*/false);
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, nullptr, &prefs, nullptr);

    m.reconcile(); // auto-Direct attempt fails -> failure recorded + Standard stored.
    REQUIRE(gw.claimCalls == 1);

    m.clearChoice(kVid, kPid); // back to Auto; the prior failure still gates Direct.
    CHECK(gw.claimCalls == 1);

    gw.devices.clear();
    m.reconcile(); // miss #1: debounced.
    m.reconcile(); // miss #2: swept, prior failure cleared with it.
    REQUIRE_FALSE(m.controllerFor(kVid, kPid).has_value());

    gw.devices = {padDevice()};
    gw.nextClaim = ClaimResult::success(-1001);
    m.reconcile(); // replug on Auto: auto-Direct runs again and now sticks.

    CHECK(gw.claimCalls == 2);
    const auto c = m.controllerFor(kVid, kPid);
    REQUIRE(c.has_value());
    CHECK(c->phase == UsbPhase::Direct);
    CHECK(c->syntheticId == -1001);
}

namespace {

// Drives one decoded report through the onReport callback on claim, so the
// manager's publish path can be asserted with no real HID IO.
class ReportingGateway : public UsbDeviceGateway {
  public:
    UsbReport report;
    int syntheticId = -1000;

    std::vector<UsbDeviceInfo> enumerate() override { return {padDevice()}; }
    ClaimResult claim(const UsbDeviceInfo& /*d*/,
                      std::function<void(const UsbReport&)> onReport) override {
        if (onReport) { onReport(report); }
        return ClaimResult::success(syntheticId);
    }
    void releaseClaim(int /*syntheticId*/) override {}
    bool isKnownFastLaneModel(int /*v*/, int /*p*/) const override { return false; }
    std::int64_t completionCount(int /*syntheticId*/) const override { return 0; }
};

} // namespace

TEST_CASE("a claimed report publishes INPUT through the processor on the claim path",
          "[usb-manager]") {
    ReportingGateway gw;
    gw.report.wButtons = dish::input::GamepadInputProcessor::Buttons::kA;
    gw.report.lt = 200;
    gw.report.lx = 12345;

    dish::input::GamepadInputProcessor processor;
    std::string gotId;
    std::uint16_t gotButtons = 0;
    std::uint8_t gotLt = 0;
    std::int16_t gotLx = 0;
    processor.setReportSender([&](const std::string& id, std::uint16_t b, std::uint8_t lt,
                                  std::uint8_t /*rt*/, std::int16_t lx, std::int16_t /*ly*/,
                                  std::int16_t /*rx*/, std::int16_t /*ry*/) {
        gotId = id;
        gotButtons = b;
        gotLt = lt;
        gotLx = lx;
    });
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, &processor, &prefs, nullptr);

    m.tryDirectMode(kVid, kPid);

    // The read loop publishes under the model vpKey string, not the synthetic id.
    const std::string expectId = std::to_string((kVid << 16) | (kPid & 0xFFFF));
    CHECK(gotId == expectId);
    CHECK(gotButtons == dish::input::GamepadInputProcessor::Buttons::kA);
    CHECK(gotLt == 200);
    CHECK(gotLx == 12345);
}

TEST_CASE("a claimed report with IMU publishes MOTION through the processor", "[usb-manager]") {
    ReportingGateway gw;
    gw.report.motionValid = true;
    gw.report.gyroX = 111;
    gw.report.accelZ = 222;

    dish::input::GamepadInputProcessor processor;
    bool gotMotion = false;
    std::int16_t gotGyroX = 0;
    std::int16_t gotAccelZ = 0;
    processor.setMotionSender([&](const std::string& /*id*/, std::int16_t gx, std::int16_t /*gy*/,
                                  std::int16_t /*gz*/, std::int16_t /*ax*/, std::int16_t /*ay*/,
                                  std::int16_t az, std::uint32_t /*dt*/) {
        gotMotion = true;
        gotGyroX = gx;
        gotAccelZ = az;
    });
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, &processor, &prefs, nullptr);

    m.tryDirectMode(kVid, kPid);

    CHECK(gotMotion);
    CHECK(gotGyroX == 111);
    CHECK(gotAccelZ == 222);
}

TEST_CASE("a claimed report without IMU does not publish MOTION", "[usb-manager]") {
    ReportingGateway gw;
    gw.report.motionValid = false;
    gw.report.gyroX = 999; // present but must be ignored (motionValid gates it).

    dish::input::GamepadInputProcessor processor;
    bool gotMotion = false;
    processor.setMotionSender([&](const std::string&, std::int16_t, std::int16_t, std::int16_t,
                                  std::int16_t, std::int16_t, std::int16_t,
                                  std::uint32_t) { gotMotion = true; });
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, &processor, &prefs, nullptr);

    m.tryDirectMode(kVid, kPid);

    CHECK_FALSE(gotMotion);
}

TEST_CASE("a claimed report with a touchpad publishes TOUCHPAD through the processor",
          "[usb-manager]") {
    ReportingGateway gw;
    gw.report.touchpadValid = true;
    gw.report.finger0Active = true;
    gw.report.finger0Id = 7;
    gw.report.finger0X = -100;
    gw.report.touchpadButton = true;

    dish::input::GamepadInputProcessor processor;
    bool gotTouch = false;
    dish::input::GamepadInputProcessor::TouchpadSample got{};
    processor.setTouchpadSender([&](const std::string& /*id*/,
                                    const dish::input::GamepadInputProcessor::TouchpadSample& s) {
        gotTouch = true;
        got = s;
    });
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore prefs(&repo);
    UsbGamepadManager m(&gw, &processor, &prefs, nullptr);

    m.tryDirectMode(kVid, kPid);

    REQUIRE(gotTouch);
    CHECK(got.finger0Active);
    CHECK(got.finger0Id == 7);
    CHECK(got.finger0X == -100);
    CHECK(got.buttonPressed);
}
