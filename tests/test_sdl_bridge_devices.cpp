// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The SDL thread hands the GUI a new device list only when the list changed: a pad attaching, a
// pad leaving, or a pad's charge moving. Battery sweeps that read an unchanged charge hand over
// nothing, so they no longer wake the GUI's rebuild.

#include "HotPathRig.h"

#include "Input/GamepadInputProcessor.h"
#include "Input/SDLGamepadBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <QObject>

#include <chrono>

namespace {

namespace hp = dish::test::hotpath;

// Runs the Qt loop, which delivers the bridge's queued signals, for `ms`.
void pumpFor(int ms) {
    hp::pumpUntil([] { return false; }, ms);
}

} // namespace

TEST_CASE("an idle pad's battery sweeps hand the GUI no new device list", "[sdl-bridge][devices]") {
    dish::input::GamepadInputProcessor processor;
    dish::input::SDLGamepadBridge bridge(&processor);
    int changes = 0;
    QObject::connect(&bridge, &dish::input::SDLGamepadBridge::devicesChanged,
                     [&changes] { ++changes; });
    SDL_GetTicks64();
    bridge.start();
    REQUIRE(hp::pumpUntil([] { return SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0; }));
    {
        const hp::VirtualPad pad("Battery sweep pad");
        REQUIRE(pad.ok());
        REQUIRE(hp::pumpUntil([&] { return !bridge.devices().isEmpty(); }));
        // The attach, then at most the first charge reading, which the next sweep takes.
        pumpFor(2500);
        const int settled = changes;
        CHECK(settled >= 1);
        CHECK(settled <= 2);
        // Three more sweeps of an unchanged charge.
        pumpFor(3200);
        CHECK(changes == settled);
        CHECK(bridge.devices().size() == 1);
    }
    REQUIRE(hp::pumpUntil([&] { return bridge.devices().isEmpty(); }));
    bridge.stop();
}
