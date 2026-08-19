// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// available = controller n transport n type n host, and the FIRST failing layer
// (Input -> Link -> Type -> Host) is the one named, because it is the one whose
// fix is actionable.

#include "core/reducer/CapabilitySolver.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::reducer::CapabilityInputs;
using dish::reducer::CapabilityRow;
using dish::reducer::CapFeature;
using dish::reducer::CapLayer;
using dish::reducer::CapVerdict;
using dish::reducer::solveCapabilities;

namespace {

// Every layer carries everything; each test then breaks exactly one thing.
// Standard is the everything-path here (SDL forwards motion, touch,
// rumble and the lightbar wherever the driver exposes them); Direct is the
// path with refusals (no output write path), pinned in its own cases below.
CapabilityInputs everythingCarries() {
    CapabilityInputs in;
    in.padMotion = true;
    in.padTouchpad = true;
    in.padRumble = true;
    in.padLightbar = true;
    in.linkDirect = false;
    in.linkUsb = true;
    in.padClaimable = true;
    in.typeResolved = true;
    in.typeMotion = true;
    in.typeTouchpad = true;
    in.typeRumble = true;
    in.typeLightbar = true;
    in.hostResolved = true;
    in.hostIsBluetooth = false;
    in.hostMouseControl = true;
    in.hostRumble = true;
    in.userMotionOn = true;
    in.userRumbleOn = true;
    in.userTouchpadMode = 1; // pad
    return in;
}

CapabilityRow rowFor(const std::vector<CapabilityRow>& rows, CapFeature f) {
    for (const auto& r : rows) {
        if (r.feature == f) { return r; }
    }
    FAIL("feature missing from the solved rows");
    return {};
}

} // namespace

TEST_CASE("capability solver: the seven features come back in the fixed render order",
          "[capability][solver]") {
    const auto rows = solveCapabilities(everythingCarries());
    REQUIRE(rows.size() == 7);
    REQUIRE(rows[0].feature == CapFeature::Gamepad);
    REQUIRE(rows[1].feature == CapFeature::Triggers);
    REQUIRE(rows[2].feature == CapFeature::Motion);
    REQUIRE(rows[3].feature == CapFeature::Touchpad);
    REQUIRE(rows[4].feature == CapFeature::Mouse);
    REQUIRE(rows[5].feature == CapFeature::Rumble);
    REQUIRE(rows[6].feature == CapFeature::Lightbar);
}

TEST_CASE("capability solver: everything carrying reads Available and blames nobody",
          "[capability][solver]") {
    const auto rows = solveCapabilities(everythingCarries());
    for (const auto& r : rows) {
        if (r.feature == CapFeature::Mouse) { continue; } // touchpad mode is `pad`
        REQUIRE(r.verdict == CapVerdict::Available);
        REQUIRE_FALSE(r.hasFailingLayer);
        REQUIRE(r.inOk);
        REQUIRE(r.linkOk);
        REQUIRE(r.typeOk);
        REQUIRE(r.hostOk);
    }
}

TEST_CASE("capability solver: a pad with no gyro fails Motion on the Input layer",
          "[capability][solver]") {
    auto in = everythingCarries();
    in.padMotion = false;
    const auto motion = rowFor(solveCapabilities(in), CapFeature::Motion);
    REQUIRE(motion.verdict == CapVerdict::Unavailable);
    REQUIRE(motion.hasFailingLayer);
    REQUIRE(motion.failingLayer == CapLayer::Input);
    REQUIRE_FALSE(motion.inOk);
    // Later layers still report their own truth; only the verdict names the
    // first failure.
    REQUIRE(motion.linkOk);
    REQUIRE(motion.typeOk);
    REQUIRE(motion.hostOk);
}

TEST_CASE("capability solver: the Standard link refuses nothing the pad carries",
          "[capability][solver]") {
    // SDL forwards motion, touch, rumble and the lightbar on Standard; a pad
    // whose driver lacks one shows it at the Input layer, never the Link.
    auto in = everythingCarries();
    in.linkDirect = false;
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::Gamepad, CapFeature::Triggers, CapFeature::Motion,
                         CapFeature::Touchpad, CapFeature::Rumble, CapFeature::Lightbar}) {
        REQUIRE(rowFor(rows, f).linkOk);
        REQUIRE(rowFor(rows, f).verdict == CapVerdict::Available);
    }
}

TEST_CASE("capability solver: a Direct claim fails rumble and lightbar on the Link layer",
          "[capability][solver]") {
    // The dish-android #146 rule, path-resolved for the desktop: a capability shows
    // only where it fires. A raw-HID claim reads everything but drives nothing
    // back (no output write path), so rumble and the lightbar refuse at Link —
    // the pad still HAS them, which is why the blame is not Input.
    auto in = everythingCarries();
    in.linkDirect = true;
    const auto rows = solveCapabilities(in);

    for (const auto f : {CapFeature::Rumble, CapFeature::Lightbar}) {
        const auto row = rowFor(rows, f);
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.hasFailingLayer);
        REQUIRE(row.failingLayer == CapLayer::Link);
        REQUIRE(row.inOk);
        REQUIRE_FALSE(row.linkOk);
    }
    for (const auto f :
         {CapFeature::Gamepad, CapFeature::Triggers, CapFeature::Motion, CapFeature::Touchpad}) {
        REQUIRE(rowFor(rows, f).linkOk);
        REQUIRE(rowFor(rows, f).verdict == CapVerdict::Available);
    }
}

TEST_CASE("capability solver: the per-path rumble matrix", "[capability][solver]") {
    // The android CapabilityComposer per-path matrix, re-derived: the probe is
    // authoritative on Standard, the (absent) write path on Direct.
    auto in = everythingCarries();

    SECTION("pad with motors on Standard -> available") {
        in.linkDirect = false;
        REQUIRE(rowFor(solveCapabilities(in), CapFeature::Rumble).verdict == CapVerdict::Available);
    }
    SECTION("pad with motors on Direct -> unavailable at Link") {
        in.linkDirect = true;
        const auto row = rowFor(solveCapabilities(in), CapFeature::Rumble);
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.failingLayer == CapLayer::Link);
    }
    SECTION("pad without motors (Steam Controller) -> unavailable at Input on either path") {
        in.padRumble = false;
        for (const bool direct : {false, true}) {
            in.linkDirect = direct;
            const auto row = rowFor(solveCapabilities(in), CapFeature::Rumble);
            REQUIRE(row.verdict == CapVerdict::Unavailable);
            REQUIRE(row.failingLayer == CapLayer::Input);
        }
    }
}

TEST_CASE("capability solver: an unresolved catalog reads Pending everywhere with no blame",
          "[capability][solver]") {
    auto in = everythingCarries();
    in.typeResolved = false;
    const auto rows = solveCapabilities(in);
    for (const auto& r : rows) {
        REQUIRE(r.verdict == CapVerdict::Pending);
        REQUIRE_FALSE(r.hasFailingLayer);
        // An unread layer must never be able to draw a cross.
        REQUIRE(r.typeOk);
    }
}

TEST_CASE("capability solver: no destination at all reads Pending", "[capability][solver]") {
    auto in = everythingCarries();
    in.hostResolved = false;
    const auto rows = solveCapabilities(in);
    for (const auto& r : rows) {
        REQUIRE(r.verdict == CapVerdict::Pending);
        REQUIRE_FALSE(r.hasFailingLayer);
        REQUIRE(r.hostOk);
    }
}

TEST_CASE("capability solver: a type that drops a feature fails it on the Type layer",
          "[capability][solver]") {
    // A type can advertise a feature as unsupported even when the pad and the
    // transport both carry it (switchpro / analogTriggers).
    auto in = everythingCarries();
    in.typeMotion = false;
    const auto motion = rowFor(solveCapabilities(in), CapFeature::Motion);
    REQUIRE(motion.verdict == CapVerdict::Unavailable);
    REQUIRE(motion.failingLayer == CapLayer::Type);
    REQUIRE(motion.inOk);
    REQUIRE(motion.linkOk);
    REQUIRE_FALSE(motion.typeOk);
}

TEST_CASE("capability solver: a Bluetooth host fails motion, touchpad and mouse on Host",
          "[capability][solver]") {
    auto in = everythingCarries();
    in.hostIsBluetooth = true;
    // A Bluetooth destination is the system gamepad layer: no catalog to
    // resolve, so it must not sit at Pending forever.
    in.typeResolved = false;
    in.userTouchpadMode = 2; // ask for mouse routing so it is not merely Off
    const auto rows = solveCapabilities(in);

    for (const auto f : {CapFeature::Motion, CapFeature::Touchpad, CapFeature::Mouse}) {
        const auto row = rowFor(rows, f);
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.failingLayer == CapLayer::Host);
        REQUIRE_FALSE(row.hostOk);
    }
    REQUIRE(rowFor(rows, CapFeature::Rumble).verdict == CapVerdict::Available);
    REQUIRE(rowFor(rows, CapFeature::Gamepad).verdict == CapVerdict::Available);
    REQUIRE(rowFor(rows, CapFeature::Lightbar).failingLayer == CapLayer::Host);
}

TEST_CASE("capability solver: an older satellite without mouse control fails Mouse on Host",
          "[capability][solver]") {
    auto in = everythingCarries();
    in.hostMouseControl = false;
    in.userTouchpadMode = 2; // mouse
    const auto mouse = rowFor(solveCapabilities(in), CapFeature::Mouse);
    REQUIRE(mouse.verdict == CapVerdict::Unavailable);
    REQUIRE(mouse.failingLayer == CapLayer::Host);
    REQUIRE(mouse.inOk);
    REQUIRE(mouse.linkOk);
    REQUIRE(mouse.typeOk);
    REQUIRE_FALSE(mouse.hostOk);
}

TEST_CASE("capability solver: a host that does not advertise rumble return fails Rumble on Host",
          "[capability][solver]") {
    auto in = everythingCarries();
    in.hostRumble = false;
    const auto rumble = rowFor(solveCapabilities(in), CapFeature::Rumble);
    REQUIRE(rumble.verdict == CapVerdict::Unavailable);
    REQUIRE(rumble.failingLayer == CapLayer::Host);
}

TEST_CASE("capability solver: Mouse needs a touchpad at the pad", "[capability][solver]") {
    auto in = everythingCarries();
    in.padTouchpad = false;
    in.userTouchpadMode = 2; // mouse
    const auto rows = solveCapabilities(in);
    const auto mouse = rowFor(rows, CapFeature::Mouse);
    REQUIRE(mouse.verdict == CapVerdict::Unavailable);
    REQUIRE(mouse.failingLayer == CapLayer::Input);
    REQUIRE_FALSE(mouse.inOk);
    REQUIRE(rowFor(rows, CapFeature::Touchpad).failingLayer == CapLayer::Input);
}

TEST_CASE("capability solver: a user switch off reads Off, never Unavailable",
          "[capability][solver]") {
    auto in = everythingCarries();
    in.userMotionOn = false;
    in.userRumbleOn = false;
    const auto rows = solveCapabilities(in);

    const auto motion = rowFor(rows, CapFeature::Motion);
    REQUIRE(motion.verdict == CapVerdict::Off);
    REQUIRE_FALSE(motion.hasFailingLayer);
    // Off is a statement about a working feature: all four layers still carry it.
    REQUIRE(motion.inOk);
    REQUIRE(motion.linkOk);
    REQUIRE(motion.typeOk);
    REQUIRE(motion.hostOk);
    REQUIRE(rowFor(rows, CapFeature::Rumble).verdict == CapVerdict::Off);
}

TEST_CASE("capability solver: the touchpad mode picks which of touchpad and mouse is on",
          "[capability][solver]") {
    auto in = everythingCarries();

    in.userTouchpadMode = 0; // off
    auto rows = solveCapabilities(in);
    REQUIRE(rowFor(rows, CapFeature::Touchpad).verdict == CapVerdict::Off);
    REQUIRE(rowFor(rows, CapFeature::Mouse).verdict == CapVerdict::Off);

    in.userTouchpadMode = 1; // pad
    rows = solveCapabilities(in);
    REQUIRE(rowFor(rows, CapFeature::Touchpad).verdict == CapVerdict::Available);
    REQUIRE(rowFor(rows, CapFeature::Mouse).verdict == CapVerdict::Off);

    in.userTouchpadMode = 2; // mouse
    rows = solveCapabilities(in);
    REQUIRE(rowFor(rows, CapFeature::Touchpad).verdict == CapVerdict::Off);
    REQUIRE(rowFor(rows, CapFeature::Mouse).verdict == CapVerdict::Available);
}

TEST_CASE("capability solver: a user switch off never masks a layer refusal",
          "[capability][solver]") {
    // No gyro AND the switch off must read Unavailable, not Off, or the user
    // goes looking for a control that would not help.
    auto in = everythingCarries();
    in.padMotion = false;
    in.userMotionOn = false;
    const auto motion = rowFor(solveCapabilities(in), CapFeature::Motion);
    REQUIRE(motion.verdict == CapVerdict::Unavailable);
    REQUIRE(motion.failingLayer == CapLayer::Input);
}

TEST_CASE("capability solver: gamepad and triggers always carry on a resolved candidate",
          "[capability][solver]") {
    CapabilityInputs in;
    in.padMotion = false;
    in.padTouchpad = false;
    in.padRumble = false;
    in.padLightbar = false;
    in.linkDirect = false;
    in.typeResolved = true;
    in.hostResolved = true;
    const auto rows = solveCapabilities(in);
    REQUIRE(rowFor(rows, CapFeature::Gamepad).verdict == CapVerdict::Available);
    REQUIRE(rowFor(rows, CapFeature::Triggers).verdict == CapVerdict::Available);
    const auto rumble = rowFor(rows, CapFeature::Rumble);
    REQUIRE(rumble.verdict == CapVerdict::Unavailable);
    REQUIRE(rumble.failingLayer == CapLayer::Input);
}
