// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The effect lists are asserted as ORDERED vectors: the coordinator runs them in
// sequence, so a reordering is a behaviour change.

#include "core/reducer/UsbPathMachine.h"

#include <catch2/catch_test_macros.hpp>

using namespace dish::reducer;
namespace ev = dish::reducer::event;
namespace fx = dish::reducer::effect;

namespace {

UsbController controller(UsbPhase phase, std::optional<int> frameworkId = std::nullopt,
                         std::optional<int> syntheticId = std::nullopt, bool hasPermission = false,
                         PathChoice desired = PathChoice::Standard, bool userInitiated = false,
                         std::optional<std::string> connId = std::nullopt,
                         std::optional<DirectClaimFailure> failure = std::nullopt) {
    UsbController c;
    c.vendorId = 0x045E;
    c.productId = 0x028E;
    c.name = "Pad";
    c.phase = phase;
    c.frameworkId = frameworkId;
    c.syntheticId = syntheticId;
    c.hasPermission = hasPermission;
    c.desired = desired;
    c.userInitiated = userInitiated;
    c.connId = std::move(connId);
    c.failure = failure;
    return c;
}

} // namespace

TEST_CASE("routed + choose direct when permitted starts a held claim", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Routed, 7, std::nullopt, true),
                          ev::Choose{PathChoice::Direct, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Claiming);
    const std::vector<UsbEffect> expected{fx::ClearFailure{}, fx::BeginHold{}, fx::Claim{}};
    CHECK(r.effects == expected);
}

TEST_CASE("routed + user choose direct without permission requests permission", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Routed, std::nullopt, std::nullopt, false),
                          ev::Choose{PathChoice::Direct, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    const std::vector<UsbEffect> expected{fx::RequestPermission{}};
    CHECK(r.effects == expected);
}

TEST_CASE("routed + auto choose direct without permission stays put", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Routed, std::nullopt, std::nullopt, false),
                          ev::Choose{PathChoice::Direct, false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->desired == PathChoice::Direct);
    CHECK(r.effects.empty());
}

TEST_CASE("routed + permission granted while wanting direct starts the claim", "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::Routed, std::nullopt, std::nullopt, false, PathChoice::Direct),
               ev::PermissionGranted{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Claiming);
    CHECK(r.next->hasPermission);
    const std::vector<UsbEffect> expected{fx::ClearFailure{}, fx::BeginHold{}, fx::Claim{}};
    CHECK(r.effects == expected);
}

TEST_CASE("routed + permission denied while wanting direct falls back to standard with the reason",
          "[usb-fsm]") {
    const auto r = reduce(
        controller(UsbPhase::Routed, std::nullopt, std::nullopt, false, PathChoice::Direct, true),
        ev::PermissionDenied{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->desired == PathChoice::Standard);
    CHECK(r.next->failure == DirectClaimFailure::PermissionDenied);
    const std::vector<UsbEffect> expected{fx::SetPref{PathChoice::Standard},
                                          fx::MarkFailure{DirectClaimFailure::PermissionDenied},
                                          fx::Notify{UsbNotice::SwitchToDirectFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("routed + framework down waits for re-enumeration", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Routed, 7), ev::FrameworkDown{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::AwaitingFramework);
    CHECK_FALSE(r.next->frameworkId.has_value());
    const std::vector<UsbEffect> expected{fx::StartTimeout{}};
    CHECK(r.effects == expected);
}

TEST_CASE("claiming + success becomes direct and clears any failure", "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::Claiming, std::nullopt, std::nullopt, true, PathChoice::Direct,
                          false, std::nullopt, DirectClaimFailure::Busy),
               ev::ClaimSucceeded{-1000});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Direct);
    CHECK(r.next->syntheticId == -1000);
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<UsbEffect> expected{fx::EndHold{}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("claiming + busy failure drops straight back to standard with the reason", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Claiming, std::nullopt, std::nullopt, false,
                                     PathChoice::Standard, true),
                          ev::ClaimFailed{DirectClaimFailure::Busy, false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->desired == PathChoice::Standard);
    CHECK(r.next->failure == DirectClaimFailure::Busy);
    const std::vector<UsbEffect> expected{fx::EndHold{}, fx::SetPref{PathChoice::Standard},
                                          fx::MarkFailure{DirectClaimFailure::Busy},
                                          fx::Notify{UsbNotice::SwitchToDirectFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("claiming + auto busy failure is silent", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Claiming, std::nullopt, std::nullopt, false,
                                     PathChoice::Standard, false),
                          ev::ClaimFailed{DirectClaimFailure::Busy, false});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    const std::vector<UsbEffect> expected{fx::EndHold{}, fx::SetPref{PathChoice::Standard},
                                          fx::MarkFailure{DirectClaimFailure::Busy}};
    CHECK(r.effects == expected);
}

TEST_CASE("claiming + init failure that stole the interface waits for the framework", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Claiming, std::nullopt, std::nullopt, false,
                                     PathChoice::Standard, true),
                          ev::ClaimFailed{DirectClaimFailure::InitFailed, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::AwaitingFramework);
    CHECK_FALSE(r.next->syntheticId.has_value());
    CHECK(r.next->failure == DirectClaimFailure::InitFailed);
    const std::vector<UsbEffect> expected{fx::StartTimeout{}};
    CHECK(r.effects == expected);
}

TEST_CASE("direct + choose standard releases and waits, keeping the placeholder", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Direct, std::nullopt, -1000),
                          ev::Choose{PathChoice::Standard, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::AwaitingFramework);
    CHECK(r.next->syntheticId == -1000);
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<UsbEffect> expected{fx::Release{}, fx::StartTimeout{}};
    CHECK(r.effects == expected);
}

TEST_CASE("awaiting from release + framework up returns to standard silently", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, -1000, false,
                                     PathChoice::Standard, false, std::string("c")),
                          ev::FrameworkUp{9});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->frameworkId == 9);
    CHECK_FALSE(r.next->syntheticId.has_value());
    const std::vector<UsbEffect> expected{fx::RemoveSynthetic{-1000}, fx::BindFramework{9},
                                          fx::SetPref{PathChoice::Standard}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("awaiting from user claim-fail + framework up returns to standard with the reason",
          "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, std::nullopt, false,
                          PathChoice::Standard, true, std::nullopt, DirectClaimFailure::InitFailed),
               ev::FrameworkUp{9});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    const std::vector<UsbEffect> expected{fx::EndHold{}, fx::BindFramework{9},
                                          fx::SetPref{PathChoice::Standard},
                                          fx::MarkFailure{DirectClaimFailure::InitFailed},
                                          fx::Notify{UsbNotice::SwitchToDirectFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("awaiting from auto claim-fail + framework up surfaces the reason without a banner",
          "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, std::nullopt, false,
                                     PathChoice::Standard, false, std::nullopt,
                                     DirectClaimFailure::InitFailed),
                          ev::FrameworkUp{9});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    const std::vector<UsbEffect> expected{fx::EndHold{}, fx::BindFramework{9},
                                          fx::SetPref{PathChoice::Standard},
                                          fx::MarkFailure{DirectClaimFailure::InitFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("awaiting from release + timeout stops in restore-stuck instead of reverting",
          "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, -1000), ev::Timeout{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::RestoreStuck);
    CHECK(r.next->syntheticId == -1000);
    const std::vector<UsbEffect> expected{fx::MarkRestoreStuck{},
                                          fx::Notify{UsbNotice::RestoreFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("awaiting from claim-fail + timeout needs replug", "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, std::nullopt), ev::Timeout{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::NeedsReplug);
    CHECK(r.next->failure == DirectClaimFailure::Dropped);
    const std::vector<UsbEffect> expected{
        fx::MarkNeedsReplug{}, fx::MarkFailure{DirectClaimFailure::Dropped},
        fx::SetPref{PathChoice::Standard}, fx::Notify{UsbNotice::NeedsReplug}};
    CHECK(r.effects == expected);
}

TEST_CASE("restore stuck + choose direct re-claims the known-good path", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::RestoreStuck, std::nullopt, -1000),
                          ev::Choose{PathChoice::Direct, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::RestoreStuck);
    CHECK(r.next->desired == PathChoice::Direct);
    const std::vector<UsbEffect> expected{fx::Reclaim{}};
    CHECK(r.effects == expected);
}

TEST_CASE("restore stuck + reclaim success becomes direct again", "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::RestoreStuck, std::nullopt, -1000), ev::ClaimSucceeded{-1001});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Direct);
    CHECK(r.next->syntheticId == -1001);
    const std::vector<UsbEffect> expected{fx::SetPref{PathChoice::Direct}, fx::ClearFailure{},
                                          fx::Notify{UsbNotice::RolledBackToDirect}};
    CHECK(r.effects == expected);
}

TEST_CASE("restore stuck + reclaim failure needs replug", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::RestoreStuck, std::nullopt, -1000),
                          ev::ClaimFailed{DirectClaimFailure::InitFailed, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::NeedsReplug);
    CHECK(r.next->failure == DirectClaimFailure::Dropped);
    const std::vector<UsbEffect> expected{fx::MarkFailure{DirectClaimFailure::Dropped},
                                          fx::Notify{UsbNotice::RestoreFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("restore stuck + choose standard retries the wait", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::RestoreStuck, std::nullopt, -1000),
                          ev::Choose{PathChoice::Standard, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::AwaitingFramework);
    CHECK(r.next->syntheticId == -1000);
    const std::vector<UsbEffect> expected{fx::ClearRestoreStuck{}, fx::StartTimeout{}};
    CHECK(r.effects == expected);
}

TEST_CASE("restore stuck + framework up finally settles on standard", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::RestoreStuck, std::nullopt, -1000, false,
                                     PathChoice::Standard, false, std::string("c")),
                          ev::FrameworkUp{9});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK_FALSE(r.next->syntheticId.has_value());
    const std::vector<UsbEffect> expected{fx::RemoveSynthetic{-1000}, fx::BindFramework{9},
                                          fx::SetPref{PathChoice::Standard},
                                          fx::ClearRestoreStuck{}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("unplug from restore stuck removes the synthetic and ends the hold", "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::RestoreStuck, std::nullopt, -1000), ev::UsbUnplugged{});
    CHECK_FALSE(r.next.has_value());
    const std::vector<UsbEffect> expected{fx::RemoveSynthetic{-1000}, fx::EndHold{}};
    CHECK(r.effects == expected);
}

TEST_CASE("needs replug + framework up returns to standard and clears the failure", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::NeedsReplug, std::nullopt, std::nullopt, false,
                                     PathChoice::Standard, false, std::string("c"),
                                     DirectClaimFailure::Dropped),
                          ev::FrameworkUp{12});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<UsbEffect> expected{fx::BindFramework{12}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("unplug removes the controller and cleans up a synthetic", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Direct, std::nullopt, -1000), ev::UsbUnplugged{});
    CHECK_FALSE(r.next.has_value());
    const std::vector<UsbEffect> expected{fx::RemoveSynthetic{-1000}};
    CHECK(r.effects == expected);
}

TEST_CASE("unplug while awaiting ends the hold", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::AwaitingFramework, std::nullopt, std::nullopt),
                          ev::UsbUnplugged{});
    CHECK_FALSE(r.next.has_value());
    const std::vector<UsbEffect> expected{fx::EndHold{}};
    CHECK(r.effects == expected);
}

TEST_CASE("unplug from routed just forgets it", "[usb-fsm]") {
    const auto r = reduce(controller(UsbPhase::Routed, 3), ev::UsbUnplugged{});
    CHECK_FALSE(r.next.has_value());
    CHECK(r.effects.empty());
}

TEST_CASE("reduce is total over every phase and event", "[usb-fsm]") {
    const std::vector<UsbEvent> events{ev::FrameworkUp{1},
                                       ev::FrameworkDown{},
                                       ev::UsbUnplugged{},
                                       ev::PermissionGranted{},
                                       ev::PermissionDenied{},
                                       ev::Choose{PathChoice::Direct, true},
                                       ev::Choose{PathChoice::Standard, true},
                                       ev::ClaimSucceeded{-2000},
                                       ev::ClaimFailed{DirectClaimFailure::Busy, false},
                                       ev::ClaimFailed{DirectClaimFailure::InitFailed, true},
                                       ev::Timeout{}};
    const std::vector<UsbPhase> phases{UsbPhase::Routed,       UsbPhase::Claiming,
                                       UsbPhase::Direct,       UsbPhase::AwaitingFramework,
                                       UsbPhase::RestoreStuck, UsbPhase::NeedsReplug};
    for (UsbPhase phase : phases) {
        for (const auto& e : events) {
            const bool held = phase == UsbPhase::Direct || phase == UsbPhase::RestoreStuck;
            const auto r = reduce(
                controller(phase, std::nullopt, held ? std::optional<int>(-1000) : std::nullopt),
                e);
            // No (phase x event) throws; a surviving controller keeps a name.
            CHECK((!r.next.has_value() || !r.next->name.empty()));
        }
    }
}

TEST_CASE("start claim clears a stale failure on the controller", "[usb-fsm]") {
    const auto r =
        reduce(controller(UsbPhase::Routed, std::nullopt, std::nullopt, true, PathChoice::Standard,
                          false, std::nullopt, DirectClaimFailure::Busy),
               ev::Choose{PathChoice::Direct, true});
    CHECK_FALSE(r.effects.empty());
    REQUIRE(r.next.has_value());
    CHECK_FALSE(r.next->failure.has_value());
}

// The SDL twin never leaves the device list across a Direct claim, so
// nothing re-emits FrameworkUp. shouldSettleAwaitingFramework is the level-
// triggered gate the coordinator uses to synthesize one.

TEST_CASE("settle predicate fires only when awaiting AND the framework is present", "[usb-fsm]") {
    CHECK(shouldSettleAwaitingFramework(UsbPhase::AwaitingFramework, /*present=*/true));
    // A genuinely-gone device must NOT settle, so Timeout->RestoreStuck stays
    // reachable instead of a spurious FrameworkUp.
    CHECK_FALSE(shouldSettleAwaitingFramework(UsbPhase::AwaitingFramework, /*present=*/false));
}

TEST_CASE("settle predicate is idempotent for non-awaiting phases", "[usb-fsm]") {
    // Nothing re-fires with the framework present, so the queued settle terminates.
    for (const UsbPhase phase : {UsbPhase::Routed, UsbPhase::Claiming, UsbPhase::Direct,
                                 UsbPhase::RestoreStuck, UsbPhase::NeedsReplug}) {
        CHECK_FALSE(shouldSettleAwaitingFramework(phase, /*present=*/true));
    }
}

TEST_CASE("settle-then-reduce drives AwaitingFramework to Routed with the standard effects",
          "[usb-fsm]") {
    const auto c = controller(UsbPhase::AwaitingFramework, std::nullopt, -1000, false,
                              PathChoice::Standard, false, std::string("c"));
    REQUIRE(shouldSettleAwaitingFramework(c.phase, /*present=*/true));
    const auto r = reduce(c, ev::FrameworkUp{42});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK_FALSE(r.next->syntheticId.has_value());
    const std::vector<UsbEffect> expected{fx::RemoveSynthetic{-1000}, fx::BindFramework{42},
                                          fx::SetPref{PathChoice::Standard}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("a truly-gone device keeps the timeout path: awaiting + timeout -> restore stuck",
          "[usb-fsm]") {
    // No FrameworkUp is synthesized, so the timer is what fires: RestoreStuck
    // (synthetic held), not Routed.
    const auto c = controller(UsbPhase::AwaitingFramework, std::nullopt, -1000);
    REQUIRE_FALSE(shouldSettleAwaitingFramework(c.phase, /*present=*/false));
    const auto r = reduce(c, ev::Timeout{});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::RestoreStuck);
    CHECK(r.next->syntheticId == -1000);
}

// ── frameworkExpected: models with no framework gamepad identity ─────────────
// The Steam Controller's stand-alone identity is a keyboard and mouse, so no
// SDL device ever follows a release; waiting for one would always time out
// into a false RestoreStuck. Ported from dish-android #154.

TEST_CASE("direct + choose standard on a frameworkless model settles straight to routed",
          "[usb-fsm]") {
    auto c = controller(UsbPhase::Direct, std::nullopt, -1000, true, PathChoice::Direct, false,
                        std::string("c"));
    c.frameworkExpected = false;
    const auto r = reduce(c, ev::Choose{PathChoice::Standard, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->desired == PathChoice::Standard);
    CHECK_FALSE(r.next->syntheticId.has_value());
    CHECK_FALSE(r.next->frameworkId.has_value());
    CHECK_FALSE(r.next->failure.has_value());
    const std::vector<UsbEffect> expected{fx::Release{}, fx::RemoveSynthetic{-1000},
                                          fx::SetPref{PathChoice::Standard}, fx::ClearFailure{}};
    CHECK(r.effects == expected);
}

TEST_CASE("direct + choose standard still awaits the framework when one is expected", "[usb-fsm]") {
    auto c = controller(UsbPhase::Direct, std::nullopt, -1000, true, PathChoice::Direct);
    REQUIRE(c.frameworkExpected);
    const auto r = reduce(c, ev::Choose{PathChoice::Standard, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::AwaitingFramework);
    const std::vector<UsbEffect> expected{fx::Release{}, fx::StartTimeout{}};
    CHECK(r.effects == expected);
}

TEST_CASE("claim-fail with a stolen framework drops to routed when none is expected", "[usb-fsm]") {
    // frameworkStolen would normally park the model in AwaitingFramework; with
    // no framework identity to wait for, the failure lands as the ordinary
    // routed-with-reason outcome instead.
    auto c =
        controller(UsbPhase::Claiming, std::nullopt, std::nullopt, true, PathChoice::Direct, true);
    c.frameworkExpected = false;
    const auto r = reduce(c, ev::ClaimFailed{DirectClaimFailure::InitFailed, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::Routed);
    CHECK(r.next->desired == PathChoice::Standard);
    CHECK(r.next->failure == DirectClaimFailure::InitFailed);
    const std::vector<UsbEffect> expected{fx::EndHold{}, fx::SetPref{PathChoice::Standard},
                                          fx::MarkFailure{DirectClaimFailure::InitFailed},
                                          fx::Notify{UsbNotice::SwitchToDirectFailed}};
    CHECK(r.effects == expected);
}

TEST_CASE("claim-fail with a stolen framework still awaits when one is expected", "[usb-fsm]") {
    auto c = controller(UsbPhase::Claiming, std::nullopt, std::nullopt, true, PathChoice::Direct);
    REQUIRE(c.frameworkExpected);
    const auto r = reduce(c, ev::ClaimFailed{DirectClaimFailure::Busy, true});
    REQUIRE(r.next.has_value());
    CHECK(r.next->phase == UsbPhase::AwaitingFramework);
    const std::vector<UsbEffect> expected{fx::StartTimeout{}};
    CHECK(r.effects == expected);
}
