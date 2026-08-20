// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Never name a local `slots` here: Qt's moc keyword macro claims that identifier
// and these tests share a vocabulary with the Qt-including seed test next door.

#include "core/reducer/BindingPresence.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace reducer = dish::reducer;
using reducer::BindingPresenceKind;
using reducer::BoundSlot;
using reducer::PresentSlot;
using reducer::resolveBindingPresence;

namespace {

PresentSlot present(const std::string& id, int vid, int pid) {
    PresentSlot s;
    s.id = id;
    s.vendorId = vid;
    s.productId = pid;
    return s;
}

// vid/pid < 0 means the pad behind the binding no longer resolves anywhere.
BoundSlot bound(const std::string& slotId, const std::string& connId, int vid = -1, int pid = -1) {
    BoundSlot b;
    b.slotId = slotId;
    b.connId = connId;
    if (vid >= 0 && pid >= 0) { b.identity = std::make_pair(vid, pid); }
    return b;
}

constexpr int kSwitchProVid = 0x057e;
constexpr int kSwitchProPid = 0x2009;
const std::string kSwitchProSynthetic = "92151817"; // (0x057e << 16) | 0x2009

} // namespace

TEST_CASE("resolveBindingPresence: a binding whose pad is present is left alone",
          "[bindingpresence]") {
    const std::vector<PresentSlot> shown = {present("sdl:0", kSwitchProVid, kSwitchProPid)};
    const std::vector<BoundSlot> bindings = {bound("sdl:0", "sat-1", kSwitchProVid, kSwitchProPid)};
    REQUIRE(resolveBindingPresence(shown, bindings).empty());
}

TEST_CASE("resolveBindingPresence: a binding whose pad is gone is unbound", "[bindingpresence]") {
    // A surviving binding is re-sent as a desired descriptor on every session
    // PUT, so the satellite re-plugs a virtual pad with no hardware behind it.
    const std::vector<BoundSlot> bindings = {bound("sdl:0", "sat-1")};
    const auto actions = resolveBindingPresence(/*present=*/{}, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Unbind);
    REQUIRE(actions[0].slotId == "sdl:0");
}

TEST_CASE("resolveBindingPresence: an unbind names the connection it dropped",
          "[bindingpresence]") {
    // AppModel turns an Unbind into a toast naming the satellite, so the
    // connection has to ride every action, not just a Migrate.
    const std::vector<BoundSlot> bindings = {bound("sdl:0", "sat-living-room")};
    const auto actions = resolveBindingPresence(/*present=*/{}, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Unbind);
    REQUIRE(actions[0].connId == "sat-living-room");
    REQUIRE(actions[0].toSlotId.empty());
}

TEST_CASE("resolveBindingPresence: a pad gone while others remain is still unbound",
          "[bindingpresence]") {
    const std::vector<PresentSlot> shown = {present("sdl:1", 0x20d6, 0xa713)};
    const std::vector<BoundSlot> bindings = {bound("sdl:0", "sat-1", kSwitchProVid, kSwitchProPid),
                                             bound("sdl:1", "sat-2", 0x20d6, 0xa713)};
    const auto actions = resolveBindingPresence(shown, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Unbind);
    REQUIRE(actions[0].slotId == "sdl:0");
}

TEST_CASE("resolveBindingPresence: a pad that moved onto its USB-direct synthetic migrates",
          "[bindingpresence]") {
    // The USB-direct claim hides the framework twin and publishes a synthetic in
    // its place. The pad is still there, so dropping the binding would tear down
    // a working stream on every path switch.
    const std::vector<PresentSlot> shown = {
        present(kSwitchProSynthetic, kSwitchProVid, kSwitchProPid)};
    const std::vector<BoundSlot> bindings = {bound("sdl:0", "sat-1", kSwitchProVid, kSwitchProPid)};
    const auto actions = resolveBindingPresence(shown, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Migrate);
    REQUIRE(actions[0].slotId == "sdl:0");
    REQUIRE(actions[0].toSlotId == kSwitchProSynthetic);
    REQUIRE(actions[0].connId == "sat-1");
}

TEST_CASE("resolveBindingPresence: a synthetic released back to the framework twin migrates back",
          "[bindingpresence]") {
    const std::vector<PresentSlot> shown = {present("sdl:0", kSwitchProVid, kSwitchProPid)};
    const std::vector<BoundSlot> bindings = {
        bound(kSwitchProSynthetic, "sat-1", kSwitchProVid, kSwitchProPid)};
    const auto actions = resolveBindingPresence(shown, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Migrate);
    REQUIRE(actions[0].toSlotId == "sdl:0");
}

TEST_CASE("resolveBindingPresence: an identity-less binding never migrates onto a stranger",
          "[bindingpresence]") {
    // 0:0 means SDL could not read the descriptor, so it is not a key: it must
    // not match another pad that also reads 0:0.
    const std::vector<PresentSlot> shown = {present("sdl:9", 0, 0)};
    const std::vector<BoundSlot> bindings = {bound("sdl:0", "sat-1", 0, 0)};
    const auto actions = resolveBindingPresence(shown, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Unbind);
}

TEST_CASE("resolveBindingPresence: a migration target that is already bound is not clobbered",
          "[bindingpresence]") {
    // Same model plugged twice: the departed slot must not steal the survivor's
    // binding.
    const std::vector<PresentSlot> shown = {
        present(kSwitchProSynthetic, kSwitchProVid, kSwitchProPid)};
    const std::vector<BoundSlot> bindings = {
        bound("sdl:0", "sat-1", kSwitchProVid, kSwitchProPid),
        bound(kSwitchProSynthetic, "sat-2", kSwitchProVid, kSwitchProPid)};
    const auto actions = resolveBindingPresence(shown, bindings);
    REQUIRE(actions.size() == 1);
    REQUIRE(actions[0].kind == BindingPresenceKind::Unbind);
    REQUIRE(actions[0].slotId == "sdl:0");
}

TEST_CASE("resolveBindingPresence: no bindings and nothing shown is a no-op", "[bindingpresence]") {
    REQUIRE(resolveBindingPresence({}, {}).empty());
}

TEST_CASE("resolveBindingPresence: actions are ordered by the departing slot id",
          "[bindingpresence]") {
    const std::vector<BoundSlot> bindings = {bound("sdl:2", "sat-c"), bound("sdl:0", "sat-a"),
                                             bound("sdl:1", "sat-b")};
    const auto actions = resolveBindingPresence({}, bindings);
    REQUIRE(actions.size() == 3);
    REQUIRE(actions[0].slotId == "sdl:0");
    REQUIRE(actions[1].slotId == "sdl:1");
    REQUIRE(actions[2].slotId == "sdl:2");
}

TEST_CASE("resolveBindingPresence: the gate is idempotent once applied", "[bindingpresence]") {
    // The AppModel rebuild re-enters after applying the actions, so a second
    // round of unbinds would loop forever.
    const std::vector<PresentSlot> shown = {
        present(kSwitchProSynthetic, kSwitchProVid, kSwitchProPid)};
    const std::vector<BoundSlot> settled = {
        bound(kSwitchProSynthetic, "sat-1", kSwitchProVid, kSwitchProPid)};
    REQUIRE(resolveBindingPresence(shown, settled).empty());
}
