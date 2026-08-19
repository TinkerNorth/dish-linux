// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Characterization of the intentionally-inert transitions plus two oddities kept
// on purpose: a non-stolen claim failure still persists Standard, and the
// NeedsReplug timeout overwrites the reason with Dropped. Pinned so that
// changing either is a visible diff.

#include "core/reducer/UsbPathMachine.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

using namespace dish::reducer;
namespace ev = dish::reducer::event;
namespace fx = dish::reducer::effect;

namespace {

UsbController controller(UsbPhase phase, std::optional<int> frameworkId = std::nullopt,
                         std::optional<int> syntheticId = std::nullopt,
                         PathChoice desired = PathChoice::Standard, bool userInitiated = false,
                         std::optional<DirectClaimFailure> failure = std::nullopt) {
    UsbController c;
    c.vendorId = 0x045E;
    c.productId = 0x028E;
    c.name = "Pad";
    c.phase = phase;
    c.frameworkId = frameworkId;
    c.syntheticId = syntheticId;
    c.desired = desired;
    c.userInitiated = userInitiated;
    c.failure = failure;
    return c;
}

bool contains(const std::vector<UsbEffect>& effects, const UsbEffect& wanted) {
    return std::find(effects.begin(), effects.end(), wanted) != effects.end();
}

} // namespace

TEST_CASE("needs replug plus choose direct records the desire but emits no recovery effect",
          "[usb-fsm-edge]") {
    const auto r = reduce(controller(UsbPhase::NeedsReplug), ev::Choose{PathChoice::Direct, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::NeedsReplug);
    CHECK(r.next->desired == PathChoice::Direct);
    // Only a physical replug (FrameworkUp) recovers, so the toggle emits no
    // Reclaim, unlike RestoreStuck + Choose(Direct).
    CHECK(r.effects.empty());
}

TEST_CASE("needs replug plus choose standard is equally inert", "[usb-fsm-edge]") {
    const auto r =
        reduce(controller(UsbPhase::NeedsReplug), ev::Choose{PathChoice::Standard, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::NeedsReplug);
    CHECK(r.effects.empty());
}

TEST_CASE("needs replug recovers only when the framework device re-enumerates", "[usb-fsm-edge]") {
    const auto r = reduce(controller(UsbPhase::NeedsReplug), ev::FrameworkUp{12});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    const std::vector<UsbEffect> expected{fx::BindFramework{12}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("claiming plus framework down is ignored, leaning on the coordinator having forgotten "
          "the framework",
          "[usb-fsm-edge]") {
    const auto r = reduce(controller(UsbPhase::Claiming), ev::FrameworkDown{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Claiming);
    CHECK(r.effects.empty());
}

TEST_CASE("direct plus framework down is ignored", "[usb-fsm-edge]") {
    const auto r = reduce(controller(UsbPhase::Direct, std::nullopt, -1000), ev::FrameworkDown{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Direct);
    CHECK(r.effects.empty());
}

TEST_CASE("a non-stolen claim failure settles on Standard and persists Standard",
          "[usb-fsm-edge]") {
    const auto r = reduce(
        controller(UsbPhase::Claiming, std::nullopt, std::nullopt, PathChoice::Standard, true),
        ev::ClaimFailed{DirectClaimFailure::Busy, false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->desired == PathChoice::Standard);
    CHECK(contains(r.effects, UsbEffect{fx::SetPref{PathChoice::Standard}}));
}

TEST_CASE("by contrast the permission-denied fallback does persist Standard", "[usb-fsm-edge]") {
    const auto r =
        reduce(controller(UsbPhase::Routed, std::nullopt, std::nullopt, PathChoice::Direct, true),
               ev::PermissionDenied{});
    CHECK(contains(r.effects, UsbEffect{fx::SetPref{PathChoice::Standard}}));
}

TEST_CASE("the timeout into NeedsReplug marks the reason as Dropped", "[usb-fsm-edge]") {
    const auto r = reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, std::nullopt,
                                     PathChoice::Standard, false, DirectClaimFailure::InitFailed),
                          ev::Timeout{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::NeedsReplug);
    CHECK(r.next->failure == DirectClaimFailure::Dropped);
    CHECK(contains(r.effects, UsbEffect{fx::MarkFailure{DirectClaimFailure::Dropped}}));
}
