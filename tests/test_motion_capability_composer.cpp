// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/MotionCapabilityComposer.h"
#include "source/store/SatelliteMotionBackendStatusStore.h"

#include "ComposerProbe.h"

#include "architecture/Observable.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

using dish::arch::Observable;
using dish::composer::deriveMotionCapabilities;
using dish::composer::MotionBindings;
using dish::composer::MotionCapability;
using dish::composer::MotionCapabilityComposer;
using dish::composer::MotionCapabilityMap;
using dish::composer::MotionConnection;
using dish::composer::MotionConnectionList;
using dish::composer::MotionDevice;
using dish::composer::MotionDeviceList;
using dish::composer::MotionEnabledMap;
using dish::source::SatelliteMotionBackendStatus;
using dish::source::SatelliteMotionBackendStatusMap;
using dish::test::ComposerProbe;

namespace {

const std::string kSlot = "9";

MotionConnection satellite(const std::string& id, bool connected = true,
                           std::map<std::string, int> types = {}) {
    return MotionConnection{id, /*isSatellite=*/true, connected, std::move(types)};
}

MotionDevice device(const std::string& slotId, bool hasGyro) {
    return MotionDevice{slotId, hasGyro};
}

std::optional<MotionCapability> latestFor(const ComposerProbe<MotionCapabilityMap>& probe,
                                          const std::string& slotId) {
    const auto& map = probe.latest();
    const auto it = map.find(slotId);
    if (it == map.end()) { return std::nullopt; }
    return it->second;
}

} // namespace

TEST_CASE("effective requires every axis true", "[motion-cap]") {
    CHECK(MotionCapability{true, true, true, true, std::nullopt}.effective());
    CHECK_FALSE(MotionCapability{false, true, true, true, std::nullopt}.effective());
    CHECK_FALSE(MotionCapability{true, false, true, true, std::nullopt}.effective());
    CHECK_FALSE(MotionCapability{true, true, false, true, std::nullopt}.effective());
}

TEST_CASE("toCapBits returns CAP_MOTION when the dish can emit motion", "[motion-cap]") {
    const MotionCapability cap{true, true, true, true, std::nullopt};
    CHECK(cap.toCapBits() == static_cast<int>(dish::proto::kCapMotion));
}

TEST_CASE("toCapBits ignores carriesOnConnection - link-down is not a cap change", "[motion-cap]") {
    const MotionCapability cap{true, false, true, true, std::nullopt};
    CHECK(cap.toCapBits() == static_cast<int>(dish::proto::kCapMotion));
}

TEST_CASE("toCapBits is zero when the user has disabled motion", "[motion-cap]") {
    const MotionCapability cap{true, true, false, true, std::nullopt};
    CHECK(cap.toCapBits() == 0);
}

TEST_CASE("toCapBits is zero on hardware without a gyro, even if enabled", "[motion-cap]") {
    const MotionCapability cap{false, true, true, true, std::nullopt};
    CHECK(cap.toCapBits() == 0);
}

TEST_CASE("toCapBits and effective are unaffected by the satellite backend status",
          "[motion-cap]") {
    // What the dish can emit does not depend on the receiver's health.
    const MotionCapability cap{true, true, true, true, SatelliteMotionBackendStatus{true, false}};
    CHECK(cap.toCapBits() == static_cast<int>(dish::proto::kCapMotion));
    CHECK(cap.effective());
}

TEST_CASE("a device with no binding is present with carriesOnConnection false", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, false)}};
    Observable<MotionBindings> bindings{{}};
    Observable<MotionConnectionList> conns{{}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    const auto cap = latestFor(probe, kSlot);
    REQUIRE(cap.has_value());
    CHECK(cap->hasGyro == false);
    CHECK(cap->carriesOnConnection == false);
    CHECK(cap->userEnabled == true);
}

TEST_CASE("physical slot hasGyro mirrors the device, both values", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device("9", true), device("11", false)}};
    Observable<MotionBindings> bindings{{}};
    Observable<MotionConnectionList> conns{{}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, "9")->hasGyro == true);
    CHECK(latestFor(probe, "11")->hasGyro == false);
}

TEST_CASE("carriesOnConnection is true when bound to a Connected satellite", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->carriesOnConnection == true);
}

TEST_CASE("carriesOnConnection is false on a non-satellite binding", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "bt-A"}}};
    // The second field is isSatellite: a Bluetooth-HID-style connection.
    Observable<MotionConnectionList> conns{{MotionConnection{"bt-A", false, true, {}}}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->carriesOnConnection == false);
}

TEST_CASE("carriesOnConnection is false while still Connecting", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", /*connected=*/false)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->carriesOnConnection == false);
}

TEST_CASE("a physical slot drops from the map when the device leaves the registry",
          "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device("9", true)}};
    Observable<MotionBindings> bindings{{}};
    Observable<MotionConnectionList> conns{{}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());
    CHECK(latestFor(probe, "9")->hasGyro == true);

    devices.set({});
    CHECK_FALSE(latestFor(probe, "9").has_value());
}

TEST_CASE("the map re-emits when the routing flips kind from satellite to bluetooth",
          "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device("9", true)}};
    Observable<MotionBindings> bindings{{{"9", "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());
    CHECK(latestFor(probe, "9")->carriesOnConnection == true);

    bindings.set({{"9", "bt-A"}});
    conns.set({MotionConnection{"bt-A", false, true, {}}});
    CHECK(latestFor(probe, "9")->carriesOnConnection == false);
}

TEST_CASE("capabilityFor returns the latest derived value, Off for an unknown slot",
          "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    const auto cap = composer.capabilityFor(kSlot);
    CHECK(cap.hasGyro);
    CHECK(cap.carriesOnConnection);
    CHECK(cap.toCapBits() == static_cast<int>(dish::proto::kCapMotion));
    CHECK(composer.capabilityFor("ghost-slot") == MotionCapability::off());
}

TEST_CASE("userEnabled defaults to true for an unwritten slot", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{}};
    Observable<MotionConnectionList> conns{{}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->userEnabled == true);
}

TEST_CASE("userEnabled flips when the motion-enabled map writes false", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{}};
    Observable<MotionConnectionList> conns{{}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());
    CHECK(latestFor(probe, kSlot)->userEnabled == true);

    enabled.set({{kSlot, false}});
    CHECK(latestFor(probe, kSlot)->userEnabled == false);
}

TEST_CASE("toCapBits is zero on a slot the user has disabled, even with a gyro", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{{kSlot, false}}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->toCapBits() == 0);
}

TEST_CASE("hostHasSinkForType is true for a PlayStation-typed satellite slot", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{
        {satellite("sat-A", true, {{kSlot, dish::proto::kControllerTypePlayStation}})}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == true);
}

TEST_CASE("hostHasSinkForType is true for a DualSense-typed satellite slot", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    // DualSense rides the same DS4 report set as PlayStation on the satellite.
    Observable<MotionConnectionList> conns{
        {satellite("sat-A", true, {{kSlot, dish::proto::kControllerTypeDualSense}})}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == true);
}

TEST_CASE("hostHasSinkForType is false for a Switch Pro-typed satellite slot", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    // Only PlayStation / DualSense ride the DS4 sink; Switch Pro does not.
    Observable<MotionConnectionList> conns{
        {satellite("sat-A", true, {{kSlot, dish::proto::kControllerTypeSwitchPro}})}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == false);
}

TEST_CASE("hostHasSinkForType is false for an Xbox-typed satellite slot", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{
        {satellite("sat-A", true, {{kSlot, dish::proto::kControllerTypeXbox}})}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == false);
}

TEST_CASE("hostHasSinkForType flips when the user changes the slot type mid-session",
          "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{
        {satellite("sat-A", true, {{kSlot, dish::proto::kControllerTypePlayStation}})}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());
    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == true);

    conns.set({satellite("sat-A", true, {{kSlot, dish::proto::kControllerTypeXbox}})});
    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == false);
}

TEST_CASE("hostHasSinkForType is true for a slot with unknown type", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == true);
}

TEST_CASE("hostHasSinkForType is true for a non-satellite-bound slot, the limit is kind",
          "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "bt-A"}}};
    // Even an Xbox type on a non-satellite connection -> sink "supported" (the
    // gate is satellite-kind only).
    Observable<MotionConnectionList> conns{
        {MotionConnection{"bt-A", false, true, {{kSlot, dish::proto::kControllerTypeXbox}}}}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->hostHasSinkForType == true);
}

// ── Composer: satelliteBackendStatus (4) ─────────────────────────────────────

TEST_CASE("satelliteBackendStatus is null when no observation has landed", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK_FALSE(latestFor(probe, kSlot)->satelliteBackendStatus.has_value());
}

TEST_CASE("satelliteBackendStatus propagates from the store for the bound connection",
          "[motion-cap]") {
    const SatelliteMotionBackendStatus status{true, false};
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{{{"sat-A", kSlot}, status}}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK(latestFor(probe, kSlot)->satelliteBackendStatus == status);
}

TEST_CASE("satelliteBackendStatus is keyed on the bound connection, wrong-conn ignored",
          "[motion-cap]") {
    const SatelliteMotionBackendStatus wrong{false, false};
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{{{"sat-B", kSlot}, wrong}}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    CHECK_FALSE(latestFor(probe, kSlot)->satelliteBackendStatus.has_value());
}

TEST_CASE("satelliteBackendStatus updates reactively when the store re-emits", "[motion-cap]") {
    const SatelliteMotionBackendStatus broken{true, false};
    const SatelliteMotionBackendStatus fixed{true, true};
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{{kSlot, "sat-A"}}};
    Observable<MotionConnectionList> conns{{satellite("sat-A", true)}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{{{"sat-A", kSlot}, broken}}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());
    CHECK(latestFor(probe, kSlot)->satelliteBackendStatus == broken);

    backend.set({{{"sat-A", kSlot}, fixed}});
    CHECK(latestFor(probe, kSlot)->satelliteBackendStatus == fixed);
}

// ── Composer: eager initial emission (1) ─────────────────────────────────────

TEST_CASE("the composer derives eagerly - the probe sees an initial snapshot", "[motion-cap]") {
    Observable<MotionDeviceList> devices{{device(kSlot, true)}};
    Observable<MotionBindings> bindings{{}};
    Observable<MotionConnectionList> conns{{}};
    Observable<MotionEnabledMap> enabled{{}};
    Observable<SatelliteMotionBackendStatusMap> backend{{}};
    MotionCapabilityComposer composer(devices, bindings, conns, enabled, backend);
    ComposerProbe<MotionCapabilityMap> probe(composer.state());

    // Eager (SharingStarted.Eagerly): the initial replay is the computed map.
    CHECK(probe.count() == 1);
    CHECK(latestFor(probe, kSlot)->hasGyro == true);
}

// ── Pure transform direct (1) ────────────────────────────────────────────────

TEST_CASE("deriveMotionCapabilities builds an entry per device", "[motion-cap]") {
    const MotionDeviceList devices{device("9", true), device("11", false)};
    const MotionBindings bindings{{"9", "sat-A"}};
    const MotionConnectionList conns{satellite("sat-A", true)};
    const auto map = deriveMotionCapabilities(devices, bindings, conns, {}, {});
    REQUIRE(map.size() == 2);
    CHECK(map.at("9").hasGyro == true);
    CHECK(map.at("9").carriesOnConnection == true);
    CHECK(map.at("11").hasGyro == false);
    CHECK(map.at("11").carriesOnConnection == false);
}
