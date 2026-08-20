// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Only the reducer and the capture predicates are pulled in: constructing the
// bridge would init SDL and hang a unit test.

#include "core/input/CaptureMode.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using dish::input::CapturePhase;
using dish::input::CaptureReduction;
using dish::input::CaptureState;
using dish::input::reduceCapture;
namespace ce = dish::input::capture_event;

namespace {

constexpr int kAxis = static_cast<int>(dish::input::CaptureKind::Axis);
constexpr int kButton = static_cast<int>(dish::input::CaptureKind::Button);
constexpr int kHat = static_cast<int>(dish::input::CaptureKind::Hat);

// Either side of the deliberate-press gate (kCaptureAxisThreshold == 16000);
// 3000 is resting jitter. Same values as test_joystick_remap_assign.cpp.
constexpr int kAxisPasses = 20000;
constexpr int kAxisSub = 3000;

const std::string kSlotA = "sdl:1";
const std::string kSlotB = "sdl:2";

CaptureState capturing(const std::string& slot, const std::string& target) {
    return CaptureState{CapturePhase::Capturing, slot, target};
}

const CaptureState kIdle{};

} // namespace

TEST_CASE("Start arms Capturing for the slot and target", "[capture][start]") {
    const CaptureReduction r = reduceCapture(kIdle, ce::Start{kSlotA, "buttonA"});
    REQUIRE(r.next.phase == CapturePhase::Capturing);
    REQUIRE(r.next.slotId == kSlotA);
    REQUIRE(r.next.target == "buttonA");
    REQUIRE(r.accepted == false);
}

TEST_CASE("matching-device button RawInput is accepted and returns to Idle", "[capture][accept]") {
    const CaptureState st = capturing(kSlotA, "buttonA");
    const CaptureReduction r =
        reduceCapture(st, ce::RawInput{kSlotA, kButton, /*index=*/3, /*value=*/1});
    REQUIRE(r.accepted == true);
    // One-shot: the coordinator must send a fresh Start to capture the next one.
    REQUIRE(r.next == kIdle);
}

TEST_CASE("matching-device axis/hat RawInput past threshold is accepted", "[capture][accept]") {
    const CaptureState st = capturing(kSlotA, "leftStickX");

    SECTION("a deliberate axis deflection assigns") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotA, kAxis, /*index=*/0, kAxisPasses});
        REQUIRE(r.accepted == true);
        REQUIRE(r.next == kIdle);
    }
    SECTION("a negative deliberate axis deflection also assigns") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotA, kAxis, /*index=*/0, -kAxisPasses});
        REQUIRE(r.accepted == true);
        REQUIRE(r.next == kIdle);
    }
    SECTION("a non-centered hat assigns") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotA, kHat, /*index=*/0, /*value=*/0x01});
        REQUIRE(r.accepted == true);
        REQUIRE(r.next == kIdle);
    }
}

TEST_CASE("OTHER-device RawInput is ignored and capture stays armed", "[capture][filter]") {
    const CaptureState st = capturing(kSlotA, "buttonA");

    SECTION("other-device button press") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotB, kButton, /*index=*/3, /*value=*/1});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == st);
    }
    SECTION("other-device full axis deflection") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotB, kAxis, /*index=*/0, kAxisPasses});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == st);
    }
}

TEST_CASE("sub-threshold RawInput from the matching device is ignored", "[capture][threshold]") {
    const CaptureState st = capturing(kSlotA, "leftStickX");

    SECTION("resting-axis jitter never self-assigns") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotA, kAxis, /*index=*/0, kAxisSub});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == st);
    }
    SECTION("a hat recenter (centered) never self-assigns") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotA, kHat, /*index=*/0, /*value=*/0x00});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == st);
    }
    SECTION("an unknown kind fails closed") {
        const CaptureReduction r =
            reduceCapture(st, ce::RawInput{kSlotA, /*kind=*/99, /*index=*/0, /*value=*/1});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == st);
    }
}

TEST_CASE("RawInput while Idle is ignored", "[capture][idle]") {
    const CaptureReduction r =
        reduceCapture(kIdle, ce::RawInput{kSlotA, kButton, /*index=*/3, /*value=*/1});
    REQUIRE(r.accepted == false);
    REQUIRE(r.next == kIdle);
}

TEST_CASE("Stop returns to Idle from each phase", "[capture][stop]") {
    SECTION("Stop while Capturing disarms and clears slot/target") {
        const CaptureReduction r = reduceCapture(capturing(kSlotA, "buttonA"), ce::Stop{});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == kIdle);
    }
    SECTION("Stop while Idle is idempotent (the missed-clear hazard is gone)") {
        const CaptureReduction r = reduceCapture(kIdle, ce::Stop{});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == kIdle);
    }
}

TEST_CASE("Start while already Capturing re-targets", "[capture][start][retarget]") {
    const CaptureState st = capturing(kSlotA, "buttonA");

    SECTION("re-target to a new output on the same slot") {
        const CaptureReduction r = reduceCapture(st, ce::Start{kSlotA, "buttonB"});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == capturing(kSlotA, "buttonB"));
    }
    SECTION("re-target to a different slot entirely") {
        const CaptureReduction r = reduceCapture(st, ce::Start{kSlotB, "leftStickX"});
        REQUIRE(r.accepted == false);
        REQUIRE(r.next == capturing(kSlotB, "leftStickX"));
    }
}
