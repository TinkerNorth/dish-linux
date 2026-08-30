// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Controller-number allocation, the CONTROLLER_MULTI active mask, and the
// hard-coded capability table every CONTROLLER_ARRIVAL is built from. These are
// the decisions a host cannot help us with: no Moonlight host reports what its
// emulated devices carry, so the table below IS the client's knowledge and a
// regression in it is invisible on the wire until a pad silently stops sending
// motion the host asked for.

#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace dish::moonlight;
namespace proto = dish::moonproto;

namespace {

SourceCapabilities everything() {
    SourceCapabilities source;
    source.rumble = true;
    source.motion = true;
    source.touchpad = true;
    source.battery = true;
    source.lightbar = true;
    return source;
}

SourceCapabilities plainPad() {
    SourceCapabilities source;
    source.rumble = true;
    return source;
}

} // namespace

TEST_CASE("controller numbers are the lowest free index, never reused live", "[moonlight][pads]") {
    PadSlots slots;
    REQUIRE(slots.assign("a") == 0);
    REQUIRE(slots.assign("b") == 1);
    REQUIRE(slots.assign("c") == 2);
    REQUIRE(slots.assign("d") == 3);
    CHECK(slots.full());
    // The fifth pad has no number, which is the whole of the four-pad limit.
    CHECK_FALSE(slots.assign("e").has_value());

    // A slot that already holds one is not handed a second.
    CHECK_FALSE(slots.assign("b").has_value());
    CHECK(slots.numberFor("b") == 1);

    // The freed index is the next one handed out, and only once it is free.
    REQUIRE(slots.release("b") == 1);
    CHECK_FALSE(slots.numberFor("b").has_value());
    CHECK(slots.assign("e") == 1);
}

TEST_CASE("the active mask has one bit per attached pad", "[moonlight][pads]") {
    PadSlots slots;
    CHECK(slots.activeMask() == 0x0000);
    slots.assign("a");
    CHECK(slots.activeMask() == 0x0001);
    slots.assign("b");
    slots.assign("c");
    CHECK(slots.activeMask() == 0x0007);
    // Clearing a bit while still naming the controller IS the unplug, so the
    // mask has to drop it before the final packet goes out.
    slots.release("b");
    CHECK(slots.activeMask() == 0x0005);
    slots.release("a");
    slots.release("c");
    CHECK(slots.activeMask() == 0x0000);
    CHECK(slots.empty());
}

TEST_CASE("an inbound event resolves to the pad holding its number", "[moonlight][pads]") {
    PadSlots slots;
    slots.assign("first");
    slots.assign("second");
    REQUIRE(slots.slotFor(0) == std::string("first"));
    REQUIRE(slots.slotFor(1) == std::string("second"));
    CHECK_FALSE(slots.slotFor(2).has_value());
    slots.release("first");
    CHECK_FALSE(slots.slotFor(0).has_value());
}

TEST_CASE("the capability ceiling per emulated type", "[moonlight][pads]") {
    // Exactly the table the reference host builds: an Xbox and a Nintendo
    // device are sticks, buttons, analog triggers and body rumble; only the
    // PlayStation device carries trigger rumble, touch, motion, battery and an
    // LED.
    CHECK(typeCapabilityCeiling(proto::kControllerTypeXbox) == 0x03);
    CHECK(typeCapabilityCeiling(proto::kControllerTypeNintendo) == 0x03);
    CHECK(typeCapabilityCeiling(proto::kControllerTypePs) == 0xFF);
    // An unknown byte is never given more than the conservative pair.
    CHECK(typeCapabilityCeiling(proto::kControllerTypeUnknown) == 0x03);
}

TEST_CASE("Nintendo carries no motion over Moonlight", "[moonlight][pads]") {
    // Unlike the satellite `switchpro` type, which is a DIFFERENT type system
    // that happens to share the name: the reference host asks for accelerometer
    // and gyro only for a PlayStation device and routes motion only into one.
    const std::uint8_t nintendo = typeCapabilityCeiling(proto::kControllerTypeNintendo);
    CHECK((nintendo & proto::kCapGyro) == 0);
    CHECK((nintendo & proto::kCapAccelerometer) == 0);
    CHECK((nintendo & proto::kCapTouchpad) == 0);
    CHECK((nintendo & proto::kCapRumble) != 0);

    const std::uint8_t ps = typeCapabilityCeiling(proto::kControllerTypePs);
    CHECK((ps & proto::kCapGyro) != 0);
    CHECK((ps & proto::kCapAccelerometer) != 0);
    CHECK((ps & proto::kCapTouchpad) != 0);
}

TEST_CASE("Auto resolves on the client, before the wire", "[moonlight][pads]") {
    // Motion present becomes PlayStation, everything else Xbox. The sentinel
    // itself never travels: it is not a wire value.
    CHECK(resolveAutoType(true) == proto::kControllerTypePs);
    CHECK(resolveAutoType(false) == proto::kControllerTypeXbox);
    CHECK(resolveControllerType(proto::kControllerTypeAuto, true) == proto::kControllerTypePs);
    CHECK(resolveControllerType(proto::kControllerTypeAuto, false) == proto::kControllerTypeXbox);

    // An explicit pick is honoured whatever the pad reports.
    CHECK(resolveControllerType(proto::kControllerTypeXbox, true) == proto::kControllerTypeXbox);
    CHECK(resolveControllerType(proto::kControllerTypeNintendo, true) ==
          proto::kControllerTypeNintendo);
    CHECK(resolveControllerType(proto::kControllerTypePs, false) == proto::kControllerTypePs);
}

TEST_CASE("the Auto sentinel is 0xFF, and a stored 0 migrates to it", "[moonlight][pads]") {
    // 0 is CONTROLLER_TYPE_UNKNOWN on the wire, which asks the HOST to pick.
    // That is a different promise from "match the pad", so a record written
    // before the sentinel converged is migrated on read rather than sent.
    CHECK(proto::kControllerTypeAuto == 0xFF);
    CHECK(migrateControllerType(0) == proto::kControllerTypeAuto);
    CHECK(migrateControllerType(proto::kControllerTypeAuto) == proto::kControllerTypeAuto);
    // Anything outside the picker's own range is Auto too, never a wire value.
    CHECK(migrateControllerType(-1) == proto::kControllerTypeAuto);
    CHECK(migrateControllerType(9) == proto::kControllerTypeAuto);
    CHECK(migrateControllerType(255) == proto::kControllerTypeAuto);
    // The three real picks survive untouched.
    CHECK(migrateControllerType(1) == 1);
    CHECK(migrateControllerType(2) == 2);
    CHECK(migrateControllerType(3) == 3);
    // And a migrated 0 resolves the way Auto does, not the way Unknown would.
    CHECK(resolveControllerType(0, true) == proto::kControllerTypePs);
    CHECK(resolveControllerType(0, false) == proto::kControllerTypeXbox);
}

TEST_CASE("the declared bitfield is the type ceiling AND the source", "[moonlight][pads]") {
    // Declaring a capability the source cannot deliver makes the host request
    // reports that never arrive.
    CHECK(declaredCapabilities(proto::kControllerTypePs, everything()) == 0xFF);
    // A plain rumbling pad announced as a PlayStation device gets no touch, no
    // motion, no battery and no LED, because it has none.
    const std::uint8_t plainAsPs = declaredCapabilities(proto::kControllerTypePs, plainPad());
    CHECK((plainAsPs & proto::kCapTouchpad) == 0);
    CHECK((plainAsPs & proto::kCapGyro) == 0);
    CHECK((plainAsPs & proto::kCapRgbLed) == 0);
    CHECK((plainAsPs & proto::kCapAnalogTriggers) != 0);
    CHECK((plainAsPs & proto::kCapRumble) != 0);
    // A fully featured pad announced as an Xbox device is cut to the ceiling.
    CHECK(declaredCapabilities(proto::kControllerTypeXbox, everything()) == 0x03);
    CHECK(declaredCapabilities(proto::kControllerTypeNintendo, everything()) == 0x03);
    // A pad with no motors still declares its analog triggers.
    CHECK(declaredCapabilities(proto::kControllerTypeXbox, SourceCapabilities{}) ==
          proto::kCapAnalogTriggers);
}

TEST_CASE("the touchpad click rides only a live touchpad", "[moonlight][pads]") {
    // supportedButtonFlags is the whole legacy word for every type; the
    // touchpad click is the one bit that is conditional.
    CHECK(declaredButtons(0x03) == 0x0000FFFFU);
    const std::uint32_t withTouch =
        declaredButtons(declaredCapabilities(proto::kControllerTypePs, everything()));
    CHECK(withTouch == (0x0000FFFFU | proto::kBtnTouchpad));
    CHECK(declaredButtons(declaredCapabilities(proto::kControllerTypePs, plainPad())) ==
          0x0000FFFFU);
}

TEST_CASE("a second binding joins the session and never launches a second", "[moonlight][pads]") {
    // The reference count is the whole point: the session belongs to the HOST.
    CHECK(sessionNeedsStart(SessionPhase::Idle));
    CHECK(sessionNeedsStart(SessionPhase::Failed));
    for (const SessionPhase live :
         {SessionPhase::CheckingInfo, SessionPhase::Launching, SessionPhase::Rtsp,
          SessionPhase::ControlConnecting, SessionPhase::Streaming}) {
        CHECK_FALSE(sessionNeedsStart(live));
    }
}

TEST_CASE("only the last unbind hands a live app back", "[moonlight][pads]") {
    // A session that never went live always cancels: the host is holding an app
    // for us and every later attempt would be refused by our own leftovers.
    CHECK(shouldHandBackApp(/*launched=*/true, /*wentLive=*/false,
                            /*lastControllerLeft=*/false));
    CHECK(shouldHandBackApp(true, false, true));
    // A live session with controllers still on it is left alone.
    CHECK_FALSE(shouldHandBackApp(true, true, false));
    // The last one out closes the door.
    CHECK(shouldHandBackApp(true, true, true));
    // Nothing was ever launched, so there is nothing to hand back.
    CHECK_FALSE(shouldHandBackApp(false, false, true));
    CHECK_FALSE(shouldHandBackApp(false, true, true));
}

TEST_CASE("four controllers ride one session and each keeps its own type", "[moonlight][pads]") {
    // The end-to-end shape: four bindings on one host, each with its own
    // emulated type, one shared mask, and no index reused while live.
    PadSlots slots;
    std::set<std::uint8_t> numbers;
    const char* ids[] = {"pad0", "pad1", "pad2", "pad3"};
    for (const char* id : ids) {
        const auto n = slots.assign(id);
        REQUIRE(n.has_value());
        numbers.insert(*n);
    }
    CHECK(numbers.size() == kMaxPads);
    CHECK(slots.activeMask() == 0x000F);

    // Type is PER BINDING, so one host can carry a PlayStation pad and an Xbox
    // pad at the same time.
    CHECK(declaredCapabilities(resolveControllerType(proto::kControllerTypeAuto, true),
                               everything()) == 0xFF);
    CHECK(declaredCapabilities(resolveControllerType(proto::kControllerTypeAuto, false),
                               everything()) == 0x03);

    // Three leave; the session still has a rider, so the app stays up.
    CHECK(slots.release("pad0").has_value());
    CHECK(slots.release("pad1").has_value());
    CHECK(slots.release("pad2").has_value());
    CHECK_FALSE(slots.empty());
    CHECK_FALSE(shouldHandBackApp(true, true, slots.empty()));
    // The fourth leaves and the app is handed back.
    CHECK(slots.release("pad3").has_value());
    CHECK(slots.empty());
    CHECK(shouldHandBackApp(true, true, slots.empty()));
}
