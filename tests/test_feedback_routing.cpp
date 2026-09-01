// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Where a feedback message goes, and the invariant that ties it to the
// descriptor: a capability is advertised if and only if some path would carry
// it. The satellite gates its return paths on those caps, so an advertised
// capability with no target is a message sent into a hole, and a target with no
// capability is an actuator the satellite will never drive.

#include "core/reducer/FeedbackRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::reducer::FeedbackKind;
using dish::reducer::FeedbackTarget;
using dish::reducer::resolveFeedbackTarget;
using dish::reducer::slotCarriesFeedback;
using dish::reducer::SlotFeedbackInputs;

namespace {

const std::vector<FeedbackKind> kAllKinds{FeedbackKind::Rumble, FeedbackKind::Lightbar,
                                          FeedbackKind::TriggerEffects, FeedbackKind::PlayerLeds};

// A DualSense on the Standard (SDL) path: every surface in hardware.
SlotFeedbackInputs standardDualSense() {
    SlotFeedbackInputs in;
    in.usbDirect = false;
    in.padRumble = true;
    in.padLightbar = true;
    in.padTriggerEffects = true;
    in.padPlayerLeds = true;
    return in;
}

// The same pad, Direct-claimed and live.
SlotFeedbackInputs directDualSense() {
    SlotFeedbackInputs in = standardDualSense();
    in.usbDirect = true;
    in.directClaimLive = true;
    return in;
}

} // namespace

TEST_CASE("a Direct claim carries every actuator the pad has", "[feedback][routing]") {
    const auto in = directDualSense();
    for (const auto kind : kAllKinds) {
        INFO("kind " << static_cast<int>(kind));
        CHECK(resolveFeedbackTarget(in, kind) == FeedbackTarget::DirectUsb);
    }
}

TEST_CASE("the Standard path carries rumble and the lightbar and nothing else",
          "[feedback][routing]") {
    // SDL has a rumble call and an LED call. It has no adaptive-trigger or
    // player-LED call at all, so those two are structurally out of reach however
    // good the pad is -- which is why the pad flags being true here is the point.
    const auto in = standardDualSense();
    CHECK(resolveFeedbackTarget(in, FeedbackKind::Rumble) == FeedbackTarget::Standard);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::Lightbar) == FeedbackTarget::Standard);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::TriggerEffects) == FeedbackTarget::None);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::PlayerLeds) == FeedbackTarget::None);
}

TEST_CASE("a synthetic slot whose claim is gone carries nothing", "[feedback][routing]") {
    // The dangerous state: the slot still looks like a Direct pad, but the
    // claim died. Falling back to Standard here would write to a handle that
    // does not exist, and advertising anything would keep the satellite sending.
    SlotFeedbackInputs in = directDualSense();
    in.directClaimLive = false;
    for (const auto kind : kAllKinds) {
        INFO("kind " << static_cast<int>(kind));
        CHECK(resolveFeedbackTarget(in, kind) == FeedbackTarget::None);
        CHECK_FALSE(slotCarriesFeedback(in, kind));
    }
}

TEST_CASE("hardware the pad lacks is never advertised or dispatched", "[feedback][routing]") {
    // A Direct-claimed DualShock 4: lightbar yes, adaptive triggers and player
    // LEDs no.
    SlotFeedbackInputs in;
    in.usbDirect = true;
    in.directClaimLive = true;
    in.padRumble = true;
    in.padLightbar = true;
    CHECK(resolveFeedbackTarget(in, FeedbackKind::Rumble) == FeedbackTarget::DirectUsb);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::Lightbar) == FeedbackTarget::DirectUsb);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::TriggerEffects) == FeedbackTarget::None);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::PlayerLeds) == FeedbackTarget::None);
}

TEST_CASE("a Switch Pro on the Direct path drives its player lights but no colour",
          "[feedback][routing]") {
    SlotFeedbackInputs in;
    in.usbDirect = true;
    in.directClaimLive = true;
    in.padRumble = true;
    in.padPlayerLeds = true;
    CHECK(resolveFeedbackTarget(in, FeedbackKind::PlayerLeds) == FeedbackTarget::DirectUsb);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::Lightbar) == FeedbackTarget::None);
}

TEST_CASE("a pad with nothing carries nothing on either path", "[feedback][routing]") {
    SlotFeedbackInputs bare;
    for (const auto kind : kAllKinds) {
        CHECK(resolveFeedbackTarget(bare, kind) == FeedbackTarget::None);
    }
    bare.usbDirect = true;
    bare.directClaimLive = true;
    for (const auto kind : kAllKinds) {
        CHECK(resolveFeedbackTarget(bare, kind) == FeedbackTarget::None);
    }
}

TEST_CASE("a live claim flag on a Standard slot changes nothing", "[feedback][routing]") {
    // directClaimLive is only meaningful with usbDirect. A stale true must not
    // promote an SDL slot into carrying trigger effects.
    SlotFeedbackInputs in = standardDualSense();
    in.directClaimLive = true;
    CHECK(resolveFeedbackTarget(in, FeedbackKind::TriggerEffects) == FeedbackTarget::None);
    CHECK(resolveFeedbackTarget(in, FeedbackKind::Rumble) == FeedbackTarget::Standard);
}

TEST_CASE("advertising and dispatching are the same answer", "[feedback][routing]") {
    // The invariant the whole file exists for, over every combination of the
    // five inputs: slotCarriesFeedback is true exactly when a target exists.
    for (int bits = 0; bits < 32; ++bits) {
        SlotFeedbackInputs in;
        in.usbDirect = (bits & 1) != 0;
        in.directClaimLive = (bits & 2) != 0;
        in.padRumble = (bits & 4) != 0;
        in.padLightbar = (bits & 8) != 0;
        in.padTriggerEffects = (bits & 16) != 0;
        in.padPlayerLeds = (bits & 16) != 0;
        for (const auto kind : kAllKinds) {
            INFO("bits " << bits << " kind " << static_cast<int>(kind));
            const bool carries = slotCarriesFeedback(in, kind);
            const bool hasTarget = resolveFeedbackTarget(in, kind) != FeedbackTarget::None;
            CHECK(carries == hasTarget);
        }
    }
}
