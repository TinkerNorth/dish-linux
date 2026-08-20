// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which SDL/framework device ids an active USB-direct synthetic of the same model
// hides, so a pad visible to both paths streams through exactly one.

#include "core/reducer/UsbTwinDedup.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using dish::reducer::RoutedDevice;
using dish::reducer::suppressedRoutedIds;
using dish::reducer::SyntheticTwin;

namespace {

constexpr int kXboxVid = 0x045E, kXboxPid = 0x028E;
constexpr int kSonyVid = 0x054C, kDs4Pid = 0x05C4;

RoutedDevice routed(const std::string& id, int vid, int pid, bool disconnecting = false,
                    bool bluetooth = false) {
    return RoutedDevice{id, vid, pid, disconnecting, bluetooth};
}

SyntheticTwin synthetic(int vid, int pid) { return SyntheticTwin{vid, pid}; }

bool has(const std::set<std::string>& s, const std::string& id) { return s.count(id) != 0; }

} // namespace

TEST_CASE("twin-dedup empty input hides nothing", "[usb-dedup]") {
    CHECK(suppressedRoutedIds({}, {}).empty());
}

TEST_CASE("twin-dedup with no synthetics nothing is hidden", "[usb-dedup]") {
    std::vector<RoutedDevice> devices = {routed("a", kXboxVid, kXboxPid),
                                         routed("b", kSonyVid, kDs4Pid)};
    CHECK(suppressedRoutedIds({}, devices).empty());
}

TEST_CASE("twin-dedup one synthetic hides its single same-model routed twin", "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kSonyVid, kDs4Pid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:7"));
}

TEST_CASE("twin-dedup a routed controller of a different model stays visible", "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kXboxVid, kXboxPid)};
    CHECK(suppressedRoutedIds(syn, dev).empty());
}

TEST_CASE("twin-dedup Xbox routed pad is never hidden because Xbox never synthesizes",
          "[usb-dedup]") {
    // XInput hides Xbox pads from raw HID, so one is never a synthetic.
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {routed("sdl:xbox", kXboxVid, kXboxPid),
                                     routed("sdl:ds4", kSonyVid, kDs4Pid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(has(hidden, "sdl:ds4"));
    CHECK_FALSE(has(hidden, "sdl:xbox"));
}

TEST_CASE("twin-dedup two synthetics of one model hide both same-model routed twins",
          "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kXboxVid, kXboxPid), synthetic(kXboxVid, kXboxPid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kXboxVid, kXboxPid),
                                     routed("sdl:8", kXboxVid, kXboxPid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 2);
    CHECK(has(hidden, "sdl:7"));
    CHECK(has(hidden, "sdl:8"));
}

TEST_CASE("twin-dedup one synthetic with two routed twins hides the disconnecting one first",
          "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kSonyVid, kDs4Pid, /*disconnecting=*/false),
                                     routed("sdl:8", kSonyVid, kDs4Pid, /*disconnecting=*/true)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:8"));
}

TEST_CASE("twin-dedup one synthetic with two live routed twins hides exactly one", "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kSonyVid, kDs4Pid),
                                     routed("sdl:8", kSonyVid, kDs4Pid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK((has(hidden, "sdl:7") || has(hidden, "sdl:8")));
}

TEST_CASE("twin-dedup more synthetics than routed twins hides every available twin",
          "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kXboxVid, kXboxPid), synthetic(kXboxVid, kXboxPid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kXboxVid, kXboxPid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:7"));
}

TEST_CASE("twin-dedup synthetic with missing vid or pid hides nothing", "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(0, 0)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", 0, 0)};
    CHECK(suppressedRoutedIds(syn, dev).empty());
}

TEST_CASE("twin-dedup synthetics only hide twins of their own model", "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kXboxVid, kXboxPid)};
    std::vector<RoutedDevice> dev = {routed("sdl:7", kXboxVid, kXboxPid),
                                     routed("sdl:8", kSonyVid, kDs4Pid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(has(hidden, "sdl:7"));
    CHECK_FALSE(has(hidden, "sdl:8"));
}

TEST_CASE("twin-dedup detach restores the SDL twin", "[usb-dedup]") {
    std::vector<RoutedDevice> dev = {routed("sdl:7", kSonyVid, kDs4Pid)};

    std::vector<SyntheticTwin> claimed = {synthetic(kSonyVid, kDs4Pid)};
    CHECK(has(suppressedRoutedIds(claimed, dev), "sdl:7"));

    // Detach or claim-failure: no synthetics, so the twin is visible again.
    CHECK(suppressedRoutedIds({}, dev).empty());
}

TEST_CASE("twin-dedup hides the USB twin before a same-model bluetooth pad", "[usb-dedup]") {
    // Two physical DS4s: one over Bluetooth (attached first, so listed first),
    // one wired and claimed Direct. Only the wired pad's SDL twin duplicates
    // the claim's stream — the gateway refuses Bluetooth paths, so the BT pad
    // cannot be the claimed device and must keep streaming.
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {
        routed("sdl:bt", kSonyVid, kDs4Pid, /*disconnecting=*/false, /*bluetooth=*/true),
        routed("sdl:usb", kSonyVid, kDs4Pid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:usb"));
    CHECK_FALSE(has(hidden, "sdl:bt"));
}

TEST_CASE("twin-dedup transport priority holds in either input order", "[usb-dedup]") {
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {
        routed("sdl:usb", kSonyVid, kDs4Pid),
        routed("sdl:bt", kSonyVid, kDs4Pid, /*disconnecting=*/false, /*bluetooth=*/true)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:usb"));
}

TEST_CASE("twin-dedup a lone bluetooth twin is still hidden", "[usb-dedup]") {
    // Single dual-mode pad: SDL's serial dedup can keep the BT-flagged instance
    // while the claim reads the pad's USB link. The BT twin is then the model's
    // only twin and must still be suppressed, or the pad streams twice.
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {
        routed("sdl:bt", kSonyVid, kDs4Pid, /*disconnecting=*/false, /*bluetooth=*/true)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:bt"));
}

TEST_CASE("twin-dedup transport outranks disconnecting", "[usb-dedup]") {
    // Even mid-teardown, a Bluetooth row is still a wireless pad; the live USB
    // twin is the one duplicating the claim's stream.
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {
        routed("sdl:bt", kSonyVid, kDs4Pid, /*disconnecting=*/true, /*bluetooth=*/true),
        routed("sdl:usb", kSonyVid, kDs4Pid)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 1);
    CHECK(has(hidden, "sdl:usb"));
}

TEST_CASE("twin-dedup two synthetics drain the USB twins before touching bluetooth",
          "[usb-dedup]") {
    // Two claimed pads, three routed twins: the disconnecting USB twin goes
    // first, the live USB twin second, and the Bluetooth pad survives.
    std::vector<SyntheticTwin> syn = {synthetic(kSonyVid, kDs4Pid), synthetic(kSonyVid, kDs4Pid)};
    std::vector<RoutedDevice> dev = {
        routed("sdl:bt", kSonyVid, kDs4Pid, /*disconnecting=*/false, /*bluetooth=*/true),
        routed("sdl:usb-live", kSonyVid, kDs4Pid),
        routed("sdl:usb-gone", kSonyVid, kDs4Pid, /*disconnecting=*/true)};
    const auto hidden = suppressedRoutedIds(syn, dev);
    CHECK(hidden.size() == 2);
    CHECK(has(hidden, "sdl:usb-gone"));
    CHECK(has(hidden, "sdl:usb-live"));
    CHECK_FALSE(has(hidden, "sdl:bt"));
}
