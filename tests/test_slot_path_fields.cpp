// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Deliberately kept off a live UsbGamepadManager: that opens real USB/SDL and
// would hang.

#include "core/reducer/SlotPathFields.h"

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace dish::reducer;

namespace {

UsbController controller(int vid, int pid, UsbPhase phase, PathChoice desired) {
    UsbController c;
    c.vendorId = vid;
    c.productId = pid;
    c.phase = phase;
    c.desired = desired;
    return c;
}

std::map<int, UsbController> mapOf(const UsbController& c) {
    return {{slotPathVpKey(c.vendorId, c.productId), c}};
}

} // namespace

TEST_CASE("slotPathFields: a routed device reflects its controller's phase + desired",
          "[slotpath][map]") {
    const auto controllers =
        mapOf(controller(0x054C, 0x0CE6, UsbPhase::Routed, PathChoice::Standard));
    const auto f = slotPathFields(0x054C, 0x0CE6, /*bluetooth=*/false, controllers);
    REQUIRE(f.supported);
    REQUIRE(f.phase == UsbPhase::Routed);
    REQUIRE(f.desired == PathChoice::Standard);
    REQUIRE_FALSE(f.failure.has_value());
}

TEST_CASE("slotPathFields: a synthetic (Direct) device reports Direct phase + desired",
          "[slotpath][map]") {
    const auto controllers =
        mapOf(controller(0x054C, 0x0CE6, UsbPhase::Direct, PathChoice::Direct));
    const auto f = slotPathFields(0x054C, 0x0CE6, /*bluetooth=*/false, controllers);
    REQUIRE(f.supported);
    REQUIRE(f.phase == UsbPhase::Direct);
    REQUIRE(f.desired == PathChoice::Direct);
}

TEST_CASE("slotPathFields: a pad with no controller is unsupported (Xbox/XInput)",
          "[slotpath][map]") {
    // The raw-HID gateway never enumerates Xbox pads, so the map has no entry.
    const auto controllers =
        mapOf(controller(0x054C, 0x0CE6, UsbPhase::Direct, PathChoice::Direct));
    const auto f = slotPathFields(0x045E, 0x028E, /*bluetooth=*/false, controllers); // Xbox 360 pad
    REQUIRE_FALSE(f.supported);
    // Inert defaults so the QML hides the control.
    REQUIRE(f.phase == UsbPhase::Routed);
    REQUIRE(f.desired == PathChoice::Standard);
    REQUIRE_FALSE(f.failure.has_value());
}

TEST_CASE("slotPathFields: a 0/0 identity never matches (identity-less SDL slot)",
          "[slotpath][map]") {
    auto c = controller(0, 0, UsbPhase::Direct, PathChoice::Direct);
    std::map<int, UsbController> controllers = {{slotPathVpKey(0, 0), c}};
    REQUIRE_FALSE(slotPathFields(0, 0, /*bluetooth=*/false, controllers).supported);
}

TEST_CASE("slotPathFields: a Direct failure is carried through for the inline note",
          "[slotpath][map]") {
    auto c = controller(0x054C, 0x0CE6, UsbPhase::Routed, PathChoice::Direct);
    c.failure = DirectClaimFailure::Busy;
    const auto f = slotPathFields(0x054C, 0x0CE6, /*bluetooth=*/false, mapOf(c));
    REQUIRE(f.supported);
    REQUIRE(f.failure.has_value());
    REQUIRE(*f.failure == DirectClaimFailure::Busy);
}

TEST_CASE("slotPathFields: a bluetooth slot never wears its USB twin's path fields",
          "[slotpath][map]") {
    // Dual presence: the pad streams over BT while its plugged-in cable is
    // tracked under the same (vid, pid) by the claim manager. The BT card must
    // not grow a USB PATH control.
    const auto controllers =
        mapOf(controller(0x054C, 0x05C4, UsbPhase::Routed, PathChoice::Standard));
    const auto f = slotPathFields(0x054C, 0x05C4, /*bluetooth=*/true, controllers);
    REQUIRE_FALSE(f.supported);
    REQUIRE(f.phase == UsbPhase::Routed);
    REQUIRE(f.desired == PathChoice::Standard);
    REQUIRE_FALSE(f.failure.has_value());
}

TEST_CASE("slotPathFields: a bluetooth slot shows nothing even for a claimed (Direct) model",
          "[slotpath][map]") {
    // The USB twin auto-claimed Direct; its synthetic slot carries the control.
    // The BT slot stays bare rather than wearing the synthetic's phase/toggle.
    auto c = controller(0x054C, 0x05C4, UsbPhase::Direct, PathChoice::Direct);
    c.failure = DirectClaimFailure::Busy;
    const auto f = slotPathFields(0x054C, 0x05C4, /*bluetooth=*/true, mapOf(c));
    REQUIRE_FALSE(f.supported);
    REQUIRE(f.phase == UsbPhase::Routed);
    REQUIRE(f.desired == PathChoice::Standard);
    REQUIRE_FALSE(f.failure.has_value());
}

TEST_CASE("parseSyntheticSlotId: a packed vpKey string round-trips to (vid, pid)",
          "[slotpath][resolve]") {
    // The synthetic slot id is std::to_string(vpKey), the controllers map's key.
    const int vid = 0x054C;
    const int pid = 0x0CE6;
    const int key = slotPathVpKey(vid, pid);
    const auto parsed = parseSyntheticSlotId(std::to_string(key));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->first == vid);
    REQUIRE(parsed->second == pid);
}

TEST_CASE("parseSyntheticSlotId: an SDL slot id (or junk) does not parse", "[slotpath][resolve]") {
    // An SDL id is "sdl:<iid>", never all-digit, so it falls through to the
    // bridge-device lookup instead.
    REQUIRE_FALSE(parseSyntheticSlotId("sdl:3").has_value());
    REQUIRE_FALSE(parseSyntheticSlotId("").has_value());
    REQUIRE_FALSE(parseSyntheticSlotId("-5").has_value());
    REQUIRE_FALSE(parseSyntheticSlotId("12abc").has_value());
    // Past the 32-bit packed range: never a real key.
    REQUIRE_FALSE(parseSyntheticSlotId("99999999999").has_value());
}

TEST_CASE("parseSyntheticSlotId: a low-vid/pid key still splits correctly", "[slotpath][resolve]") {
    // vid 0x0001, pid 0x0002 -> key 0x00010002 -> "65538".
    const auto parsed = parseSyntheticSlotId("65538");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->first == 0x0001);
    REQUIRE(parsed->second == 0x0002);
}

TEST_CASE("slotPathSwitching: the derived loading state over the full switch flow",
          "[slotpath][switching]") {
    // Transitional FSM phases are always switching, regardless of desired/form.
    CHECK(slotPathSwitching(UsbPhase::Claiming, PathChoice::Direct, false, 0, false));
    CHECK(slotPathSwitching(UsbPhase::AwaitingFramework, PathChoice::Standard, true, 0, false));

    // Want Direct: switching until a synthetic exists AND its poll rate is
    // measured — the Hz arrives a moment after the claim.
    CHECK(slotPathSwitching(UsbPhase::Direct, PathChoice::Direct, false, 0,
                            false)); // not synthetic yet
    CHECK(slotPathSwitching(UsbPhase::Direct, PathChoice::Direct, true, 0,
                            false)); // synthetic, no rate yet
    CHECK_FALSE(slotPathSwitching(UsbPhase::Direct, PathChoice::Direct, true, 250,
                                  false)); // settled + streaming

    // Want Standard: switching while still on the synthetic; settled once back on SDL.
    CHECK(slotPathSwitching(UsbPhase::Routed, PathChoice::Standard, true, 100,
                            false)); // still synthetic
    CHECK_FALSE(
        slotPathSwitching(UsbPhase::Routed, PathChoice::Standard, false, 0, false)); // back on SDL

    CHECK_FALSE(slotPathSwitching(UsbPhase::Routed, PathChoice::Standard, false, 0, false));

    // Terminal / failure states surface an error note, never a perpetual spinner.
    CHECK_FALSE(slotPathSwitching(UsbPhase::RestoreStuck, PathChoice::Direct, false, 0, false));
    CHECK_FALSE(slotPathSwitching(UsbPhase::NeedsReplug, PathChoice::Direct, false, 0, false));
    CHECK_FALSE(
        slotPathSwitching(UsbPhase::Routed, PathChoice::Direct, false, 0, /*hasFailure=*/true));
}
