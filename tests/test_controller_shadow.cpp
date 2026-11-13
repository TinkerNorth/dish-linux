// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The SDL thread keeps a pad's state from its events instead of asking SDL's getters on every
// report. A virtual game controller is driven through a script of moves here, and after each pump
// the state the events left must be the state the getters report.

#include "Input/ControllerShadow.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <SDL.h>

#include <array>
#include <cstdint>

using dish::input::ControllerShadow;
using dish::input::GamepadSnapshot;
using dish::input::readGamepad;

namespace {

bool sameSnapshot(const GamepadSnapshot& a, const GamepadSnapshot& b) {
    return a.wButtons == b.wButtons && a.extButtons == b.extButtons && a.leftX == b.leftX &&
           a.leftY == b.leftY && a.rightX == b.rightX && a.rightY == b.rightY &&
           a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger;
}

// Every queued event goes into the shadow, as the pump cycle hands them over.
void pumpInto(ControllerShadow& shadow) {
    SDL_PumpEvents();
    std::array<SDL_Event, 64> events{};
    int got = 0;
    while ((got = SDL_PeepEvents(events.data(), static_cast<int>(events.size()), SDL_GETEVENT,
                                 SDL_FIRSTEVENT, SDL_LASTEVENT)) > 0) {
        for (int i = 0; i < got; ++i) { shadow.apply(events[static_cast<std::size_t>(i)]); }
    }
}

struct Move {
    bool button;
    int index;
    int value;
};

} // namespace

TEST_CASE("events replayed into the shadow reproduce what SDL's getters report",
          "[controller-shadow]") {
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) == 0);
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "Shadow test pad";
    const int deviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    REQUIRE(deviceIndex >= 0);
    SDL_GameController* gc = SDL_GameControllerOpen(deviceIndex);
    REQUIRE(gc != nullptr);
    SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);

    ControllerShadow shadow;
    pumpInto(shadow);
    shadow.seed(gc);
    CHECK(sameSnapshot(shadow.snapshot(), readGamepad(gc)));

    const std::array script{
        Move{false, SDL_CONTROLLER_AXIS_LEFTX, 12000},
        Move{false, SDL_CONTROLLER_AXIS_RIGHTY, -30000},
        Move{true, SDL_CONTROLLER_BUTTON_A, 1},
        Move{false, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 20000},
        Move{true, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1},
        Move{true, SDL_CONTROLLER_BUTTON_A, 0},
        Move{false, SDL_CONTROLLER_AXIS_LEFTX, -5},
        Move{true, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1},
        Move{false, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767},
        Move{true, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0},
    };
    for (const Move& move : script) {
        if (move.button) {
            SDL_JoystickSetVirtualButton(js, move.index, static_cast<Uint8>(move.value));
        } else {
            SDL_JoystickSetVirtualAxis(js, move.index, static_cast<Sint16>(move.value));
        }
        pumpInto(shadow);
        CHECK(sameSnapshot(shadow.snapshot(), readGamepad(gc)));
    }
    // Two changes inside one pump, as a diagonal stick move arrives.
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTX, 7000);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTY, 9000);
    pumpInto(shadow);
    CHECK(sameSnapshot(shadow.snapshot(), readGamepad(gc)));
    CHECK(shadow.snapshot().rightX == 7000);

    SDL_GameControllerClose(gc);
    SDL_JoystickDetachVirtual(deviceIndex);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

TEST_CASE("SDL's paddles and misc1 are the extended-buttons word, in SDL's names",
          "[controller-shadow][extended-buttons]") {
    // SDL numbers the Elite's paddles P1, P3, P2, P4 (upper right, upper left, lower
    // right, lower left from the front), which is the contract's rightPaddle1,
    // leftPaddle1, rightPaddle2, leftPaddle2; the Steam Deck's R4/L4/R5/L5 and the
    // DualSense Edge's back and Fn buttons reach SDL2 under the same names.
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) == 0);
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "Paddle test pad";
    const int deviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    REQUIRE(deviceIndex >= 0);
    SDL_GameController* gc = SDL_GameControllerOpen(deviceIndex);
    REQUIRE(gc != nullptr);
    SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);

    ControllerShadow shadow;
    pumpInto(shadow);
    shadow.seed(gc);
    REQUIRE(shadow.snapshot().extButtons == 0);

    struct Row {
        SDL_GameControllerButton button;
        std::uint32_t bit;
    };
    const std::array rows{
        Row{SDL_CONTROLLER_BUTTON_PADDLE1, dish::proto::kExtButtonRightPaddle1},
        Row{SDL_CONTROLLER_BUTTON_PADDLE2, dish::proto::kExtButtonLeftPaddle1},
        Row{SDL_CONTROLLER_BUTTON_PADDLE3, dish::proto::kExtButtonRightPaddle2},
        Row{SDL_CONTROLLER_BUTTON_PADDLE4, dish::proto::kExtButtonLeftPaddle2},
        Row{SDL_CONTROLLER_BUTTON_MISC1, dish::proto::kExtButtonMisc1},
    };
    for (const Row& row : rows) {
        SDL_JoystickSetVirtualButton(js, row.button, 1);
        pumpInto(shadow);
        CHECK(shadow.snapshot().extButtons == row.bit);
        CHECK(shadow.snapshot().wButtons == 0);
        CHECK(sameSnapshot(shadow.snapshot(), readGamepad(gc)));
        SDL_JoystickSetVirtualButton(js, row.button, 0);
        pumpInto(shadow);
        CHECK(shadow.snapshot().extButtons == 0);
    }
    // The touchpad button is SDL's own and has no bit here: it stays off the word.
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_TOUCHPAD, 1);
    pumpInto(shadow);
    CHECK(shadow.snapshot().extButtons == 0);
    CHECK(sameSnapshot(shadow.snapshot(), readGamepad(gc)));

    SDL_GameControllerClose(gc);
    SDL_JoystickDetachVirtual(deviceIndex);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

TEST_CASE("the shadow ignores events that are not an axis or a button", "[controller-shadow]") {
    ControllerShadow shadow;
    SDL_Event ev{};
    ev.type = SDL_CONTROLLERTOUCHPADMOTION;
    CHECK_FALSE(shadow.apply(ev));
    ev.type = SDL_CONTROLLERAXISMOTION;
    ev.caxis.axis = SDL_CONTROLLER_AXIS_MAX;
    CHECK_FALSE(shadow.apply(ev));
    ev.caxis.axis = SDL_CONTROLLER_AXIS_LEFTY;
    ev.caxis.value = 4242;
    CHECK(shadow.apply(ev));
    CHECK(shadow.snapshot().leftY == 4242);
}
