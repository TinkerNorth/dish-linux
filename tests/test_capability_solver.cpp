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
// rumble and the lightbar wherever the driver exposes them, but has no call at
// all for adaptive triggers or player LEDs); Direct writes OUT reports too, so
// it carries every actuator the pad has. Both are pinned in their own cases.
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
    in.padTriggerEffects = true;
    in.padPlayerLeds = true;
    in.typeTriggerEffects = true;
    in.typePlayerLeds = true;
    // Controller audio: the pad has routes (a Wave-2 fact, forced true here so
    // the fold is exercised ahead of the routes existing), the type is a Sony
    // composite and the probe said the host carries both directions.
    in.padMic = true;
    in.padSpeaker = true;
    in.typeMic = true;
    in.typeSpeaker = true;
    in.hostMic = true;
    in.hostSpeaker = true;
    in.hostResolved = true;
    in.hostIsBluetooth = false;
    in.hostMouseControl = true;
    in.hostRumble = true;
    in.userMotionOn = true;
    in.userRumbleOn = true;
    in.userMicOn = true;
    in.userSpeakerOn = true;
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

TEST_CASE("capability solver: the eleven features come back in the fixed render order",
          "[capability][solver]") {
    // Declaration order is the render order both surfaces use, so a row inserted
    // in the middle would silently reshuffle two UI tables. The protocol-2
    // additions are appended after the lightbar for that reason, the audio pair
    // last of all.
    const auto rows = solveCapabilities(everythingCarries());
    REQUIRE(rows.size() == 11);
    REQUIRE(rows[0].feature == CapFeature::Gamepad);
    REQUIRE(rows[1].feature == CapFeature::Triggers);
    REQUIRE(rows[2].feature == CapFeature::Motion);
    REQUIRE(rows[3].feature == CapFeature::Touchpad);
    REQUIRE(rows[4].feature == CapFeature::Mouse);
    REQUIRE(rows[5].feature == CapFeature::Rumble);
    REQUIRE(rows[6].feature == CapFeature::Lightbar);
    REQUIRE(rows[7].feature == CapFeature::TriggerEffects);
    REQUIRE(rows[8].feature == CapFeature::PlayerLeds);
    REQUIRE(rows[9].feature == CapFeature::Mic);
    REQUIRE(rows[10].feature == CapFeature::Speaker);
}

TEST_CASE("capability solver: everything carrying reads Available and blames nobody",
          "[capability][solver]") {
    // The base is the Standard path, where the two protocol-2 actuators have no
    // SDL call to reach them however good the pad is; their Available case is
    // the Direct one below. Skipping them here rather than weakening the base
    // keeps this case honest about what "everything carries" means per path.
    const auto rows = solveCapabilities(everythingCarries());
    for (const auto& r : rows) {
        if (r.feature == CapFeature::Mouse) { continue; } // touchpad mode is `pad`
        if (r.feature == CapFeature::TriggerEffects || r.feature == CapFeature::PlayerLeds) {
            continue;
        }
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

TEST_CASE("capability solver: a Direct claim carries every actuator the pad has",
          "[capability][solver]") {
    // A raw-HID claim now writes OUT reports as well as reading IN ones, so the
    // Link layer stops refusing rumble and the lightbar. That pair is exactly
    // what used to fail here, and lifting it is the point of the output path.
    auto in = everythingCarries();
    in.linkDirect = true;
    const auto rows = solveCapabilities(in);

    for (const auto f : {CapFeature::Gamepad, CapFeature::Triggers, CapFeature::Motion,
                         CapFeature::Touchpad, CapFeature::Rumble, CapFeature::Lightbar,
                         CapFeature::TriggerEffects, CapFeature::PlayerLeds}) {
        INFO("feature " << static_cast<int>(f));
        REQUIRE(rowFor(rows, f).linkOk);
        REQUIRE(rowFor(rows, f).verdict == CapVerdict::Available);
    }
}

TEST_CASE("capability solver: the Standard path cannot carry the protocol-2 actuators",
          "[capability][solver]") {
    // SDL has a rumble call and an LED call and nothing else: no adaptive
    // trigger API, no player-LED API. The pad still HAS the hardware, which is
    // why the blame is Link and not Input.
    auto in = everythingCarries();
    in.linkDirect = false;
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::TriggerEffects, CapFeature::PlayerLeds}) {
        const auto row = rowFor(rows, f);
        INFO("feature " << static_cast<int>(f));
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.hasFailingLayer);
        REQUIRE(row.failingLayer == CapLayer::Link);
        REQUIRE(row.inOk);
        REQUIRE_FALSE(row.linkOk);
    }
}

TEST_CASE("capability solver: the per-path rumble matrix", "[capability][solver]") {
    // The android CapabilityComposer per-path matrix, re-derived: the probe is
    // authoritative on Standard, the claim's OUT report path on Direct.
    auto in = everythingCarries();

    SECTION("pad with motors on Standard -> available") {
        in.linkDirect = false;
        REQUIRE(rowFor(solveCapabilities(in), CapFeature::Rumble).verdict == CapVerdict::Available);
    }
    SECTION("pad with motors on Direct -> available too") {
        in.linkDirect = true;
        REQUIRE(rowFor(solveCapabilities(in), CapFeature::Rumble).verdict == CapVerdict::Available);
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

TEST_CASE("capability solver: hardware the pad lacks blames Input, not Link",
          "[capability][solver]") {
    // A Direct-claimed DualShock 4 has no adaptive triggers and no player LEDs.
    // The path could carry them; the pad has none, so the actionable layer is
    // Input and no amount of switching paths would help.
    auto in = everythingCarries();
    in.linkDirect = true;
    in.padTriggerEffects = false;
    in.padPlayerLeds = false;
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::TriggerEffects, CapFeature::PlayerLeds}) {
        const auto row = rowFor(rows, f);
        INFO("feature " << static_cast<int>(f));
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.failingLayer == CapLayer::Input);
    }
}

TEST_CASE("capability solver: the catalog type can still refuse the new actuators",
          "[capability][solver]") {
    // A user who emulated an Xbox 360 pad on a DualSense gets no adaptive
    // triggers, because the TYPE has none however good the hardware is.
    auto in = everythingCarries();
    in.linkDirect = true;
    in.typeTriggerEffects = false;
    in.typePlayerLeds = false;
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::TriggerEffects, CapFeature::PlayerLeds}) {
        const auto row = rowFor(rows, f);
        INFO("feature " << static_cast<int>(f));
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.failingLayer == CapLayer::Type);
    }
}

TEST_CASE("capability solver: a Bluetooth host carries none of the new actuators",
          "[capability][solver]") {
    // The system gamepad layer has a rumble channel and nothing else, so the
    // Host layer refuses both even on a Direct-claimed DualSense.
    auto in = everythingCarries();
    in.linkDirect = true;
    in.hostIsBluetooth = true;
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::TriggerEffects, CapFeature::PlayerLeds, CapFeature::Mic,
                         CapFeature::Speaker}) {
        const auto row = rowFor(rows, f);
        INFO("feature " << static_cast<int>(f));
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.failingLayer == CapLayer::Host);
    }
    REQUIRE(rowFor(rows, CapFeature::Rumble).verdict == CapVerdict::Available);
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

// ── Controller audio ─────────────────────────────────────────────────────────

TEST_CASE("capability solver: audio carries when every layer says yes, on either path",
          "[capability][solver][audio]") {
    // The streams ride the pad's own USB-audio endpoints — a separate interface
    // from the HID path — so unlike the trigger/LED pair the Link layer never
    // refuses them and Standard vs Direct changes nothing.
    auto in = everythingCarries();
    for (const bool direct : {false, true}) {
        in.linkDirect = direct;
        const auto rows = solveCapabilities(in);
        for (const auto f : {CapFeature::Mic, CapFeature::Speaker}) {
            INFO("direct " << direct << " feature " << static_cast<int>(f));
            const auto row = rowFor(rows, f);
            REQUIRE(row.verdict == CapVerdict::Available);
            REQUIRE(row.linkOk);
        }
    }
}

TEST_CASE("capability solver: the audio fold matrix names the first refusing layer",
          "[capability][solver][audio]") {
    auto in = everythingCarries();

    SECTION("no route at the pad -> Input") {
        in.padMic = false;
        in.padSpeaker = false;
        const auto rows = solveCapabilities(in);
        for (const auto f : {CapFeature::Mic, CapFeature::Speaker}) {
            const auto row = rowFor(rows, f);
            REQUIRE(row.verdict == CapVerdict::Unavailable);
            REQUIRE(row.failingLayer == CapLayer::Input);
        }
    }
    SECTION("a type without audio endpoints -> Type") {
        // An Xbox 360 emulation on a DualSense: the materialized pad has no
        // audio function however good the physical hardware is.
        in.typeMic = false;
        in.typeSpeaker = false;
        const auto rows = solveCapabilities(in);
        for (const auto f : {CapFeature::Mic, CapFeature::Speaker}) {
            const auto row = rowFor(rows, f);
            REQUIRE(row.verdict == CapVerdict::Unavailable);
            REQUIRE(row.failingLayer == CapLayer::Type);
        }
    }
    SECTION("a host that switched audio off -> Host") {
        in.hostMic = false;
        in.hostSpeaker = false;
        const auto rows = solveCapabilities(in);
        for (const auto f : {CapFeature::Mic, CapFeature::Speaker}) {
            const auto row = rowFor(rows, f);
            REQUIRE(row.verdict == CapVerdict::Unavailable);
            REQUIRE(row.failingLayer == CapLayer::Host);
        }
    }
    SECTION("the directions are independent at every layer") {
        in.hostMic = false; // speaker-only host
        const auto rows = solveCapabilities(in);
        REQUIRE(rowFor(rows, CapFeature::Mic).verdict == CapVerdict::Unavailable);
        REQUIRE(rowFor(rows, CapFeature::Speaker).verdict == CapVerdict::Available);
    }
}

TEST_CASE("capability solver: an unprobed host refuses audio rather than waiting",
          "[capability][solver][audio]") {
    // hostMic/hostSpeaker have no Pending state on purpose: audio is opt-in,
    // so before a probe lands the honest verdict is "not carried" — and it must
    // not read as the uniform Pending the unresolved-catalog case shows.
    auto in = everythingCarries();
    in.hostMic = false;
    in.hostSpeaker = false;
    const auto rows = solveCapabilities(in);
    REQUIRE(rowFor(rows, CapFeature::Mic).verdict == CapVerdict::Unavailable);
    REQUIRE(rowFor(rows, CapFeature::Speaker).verdict == CapVerdict::Unavailable);
    REQUIRE(rowFor(rows, CapFeature::Motion).verdict == CapVerdict::Available);
}

TEST_CASE("capability solver: the audio user switches read Off, never Unavailable",
          "[capability][solver][audio]") {
    auto in = everythingCarries();
    in.userMicOn = false;
    in.userSpeakerOn = false;
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::Mic, CapFeature::Speaker}) {
        const auto row = rowFor(rows, f);
        REQUIRE(row.verdict == CapVerdict::Off);
        REQUIRE_FALSE(row.hasFailingLayer);
        REQUIRE(row.inOk);
        REQUIRE(row.hostOk);
    }
}

TEST_CASE("capability solver: default inputs advertise no audio anywhere",
          "[capability][solver][audio]") {
    // The Wave-1 end-to-end truth: the pad-route seam answers false for every
    // slot until the audio engines land, so with everything else carrying, the
    // audio rows still refuse at Input and nothing ever advertises a cap.
    auto in = everythingCarries();
    in.padMic = CapabilityInputs{}.padMic;         // the real default,
    in.padSpeaker = CapabilityInputs{}.padSpeaker; // not a test's idea of it
    const auto rows = solveCapabilities(in);
    for (const auto f : {CapFeature::Mic, CapFeature::Speaker}) {
        const auto row = rowFor(rows, f);
        REQUIRE(row.verdict == CapVerdict::Unavailable);
        REQUIRE(row.failingLayer == CapLayer::Input);
    }
    // And the defaults themselves are pinned: flipping them is Wave 2's move,
    // made deliberately, not by a refactor.
    CHECK_FALSE(CapabilityInputs{}.padMic);
    CHECK_FALSE(CapabilityInputs{}.padSpeaker);
    CHECK_FALSE(CapabilityInputs{}.hostMic);
    CHECK_FALSE(CapabilityInputs{}.hostSpeaker);
    // The user defaults mirror the stores': mic off (privacy), speaker on.
    CHECK_FALSE(CapabilityInputs{}.userMicOn);
    CHECK(CapabilityInputs{}.userSpeakerOn);
}
