// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "JoystickMapping.h"

// Clang has _m_prefetch as a builtin, and SDL_endian.h defines it unless an intrinsics header
// came first.
#include <intrin.h>

#include <SDL_events.h>

#include <array>
#include <cstdint>

namespace dish::input {

// A game controller's state as its own events left it: seeded with one full read when the pad
// opens, then moved by each axis and button event, so the SDL thread never asks SDL's getters for
// it again. Owned by the SDL thread.
class ControllerShadow {
  public:
    void seed(SDL_GameController* gc);

    // True when the event was an axis or button change, which is all this keeps.
    bool apply(const SDL_Event& ev);

    GamepadSnapshot snapshot() const;
    bool pressed(SDL_GameControllerButton button) const;

  private:
    std::array<std::int16_t, SDL_CONTROLLER_AXIS_MAX> axes_{};
    std::uint32_t buttons_ = 0;
};

// The same snapshot, read straight from SDL's getters.
GamepadSnapshot readGamepad(SDL_GameController* gc);

} // namespace dish::input
