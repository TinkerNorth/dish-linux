// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Input/ControllerShadow.h"

#include "core/model/Protocol.h"

#include <SDL_gamecontroller.h>

#include <utility>

namespace dish::input {

namespace proto = dish::proto;

namespace {

static_assert(SDL_CONTROLLER_BUTTON_MAX <= 32, "every button fits one word of bits");

// SDL button to XUSB bit, one row each, so the two cannot drift apart.
constexpr std::pair<SDL_GameControllerButton, std::uint16_t> kButtonBits[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, GamepadInputProcessor::Buttons::kDpadUp},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, GamepadInputProcessor::Buttons::kDpadDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, GamepadInputProcessor::Buttons::kDpadLeft},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, GamepadInputProcessor::Buttons::kDpadRight},
    {SDL_CONTROLLER_BUTTON_START, GamepadInputProcessor::Buttons::kStart},
    {SDL_CONTROLLER_BUTTON_BACK, GamepadInputProcessor::Buttons::kBack},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, GamepadInputProcessor::Buttons::kLeftThumb},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, GamepadInputProcessor::Buttons::kRightThumb},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GamepadInputProcessor::Buttons::kLeftShoulder},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GamepadInputProcessor::Buttons::kRightShoulder},
    {SDL_CONTROLLER_BUTTON_GUIDE, GamepadInputProcessor::Buttons::kGuide},
    {SDL_CONTROLLER_BUTTON_A, GamepadInputProcessor::Buttons::kA},
    {SDL_CONTROLLER_BUTTON_B, GamepadInputProcessor::Buttons::kB},
    {SDL_CONTROLLER_BUTTON_X, GamepadInputProcessor::Buttons::kX},
    {SDL_CONTROLLER_BUTTON_Y, GamepadInputProcessor::Buttons::kY},
};

// SDL button to extended-buttons bit, SDL's own names: its paddles 1 to 4 are the Elite's
// P1, P3, P2, P4 and the Steam Deck's R4, L4, R5, L5, which the contract calls rightPaddle1,
// leftPaddle1, rightPaddle2, leftPaddle2; misc1 is the Deck's QAM, Capture, Share.
constexpr std::pair<SDL_GameControllerButton, std::uint32_t> kExtendedButtonBits[] = {
    {SDL_CONTROLLER_BUTTON_PADDLE1, proto::kExtButtonRightPaddle1},
    {SDL_CONTROLLER_BUTTON_PADDLE2, proto::kExtButtonLeftPaddle1},
    {SDL_CONTROLLER_BUTTON_PADDLE3, proto::kExtButtonRightPaddle2},
    {SDL_CONTROLLER_BUTTON_PADDLE4, proto::kExtButtonLeftPaddle2},
    {SDL_CONTROLLER_BUTTON_MISC1, proto::kExtButtonMisc1},
};

std::uint32_t bitOf(int button) { return std::uint32_t{1} << static_cast<unsigned>(button); }

} // namespace

void ControllerShadow::seed(SDL_GameController* gc) {
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
        axes_[static_cast<std::size_t>(axis)] =
            SDL_GameControllerGetAxis(gc, static_cast<SDL_GameControllerAxis>(axis));
    }
    buttons_ = 0;
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
        if (SDL_GameControllerGetButton(gc, static_cast<SDL_GameControllerButton>(button)) != 0) {
            buttons_ |= bitOf(button);
        }
    }
}

bool ControllerShadow::apply(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis >= SDL_CONTROLLER_AXIS_MAX) { return false; }
        axes_[ev.caxis.axis] = ev.caxis.value;
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (ev.cbutton.button >= SDL_CONTROLLER_BUTTON_MAX) { return false; }
        if (ev.cbutton.state == SDL_PRESSED) {
            buttons_ |= bitOf(ev.cbutton.button);
        } else {
            buttons_ &= ~bitOf(ev.cbutton.button);
        }
        return true;
    default:
        return false;
    }
}

bool ControllerShadow::pressed(SDL_GameControllerButton button) const {
    return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX && (buttons_ & bitOf(button)) != 0;
}

GamepadSnapshot ControllerShadow::snapshot() const {
    std::uint16_t buttons = 0;
    for (const auto& [button, bit] : kButtonBits) {
        if (pressed(button)) { buttons |= bit; }
    }
    std::uint32_t extButtons = 0;
    for (const auto& [button, bit] : kExtendedButtonBits) {
        if (pressed(button)) { extButtons |= bit; }
    }
    return {.wButtons = buttons,
            .extButtons = extButtons,
            .leftX = axes_[SDL_CONTROLLER_AXIS_LEFTX],
            .leftY = axes_[SDL_CONTROLLER_AXIS_LEFTY],
            .rightX = axes_[SDL_CONTROLLER_AXIS_RIGHTX],
            .rightY = axes_[SDL_CONTROLLER_AXIS_RIGHTY],
            .leftTrigger = axes_[SDL_CONTROLLER_AXIS_TRIGGERLEFT],
            .rightTrigger = axes_[SDL_CONTROLLER_AXIS_TRIGGERRIGHT]};
}

GamepadSnapshot readGamepad(SDL_GameController* gc) {
    std::uint16_t buttons = 0;
    for (const auto& [button, bit] : kButtonBits) {
        if (SDL_GameControllerGetButton(gc, button) != 0) { buttons |= bit; }
    }
    std::uint32_t extButtons = 0;
    for (const auto& [button, bit] : kExtendedButtonBits) {
        if (SDL_GameControllerGetButton(gc, button) != 0) { extButtons |= bit; }
    }
    return {.wButtons = buttons,
            .extButtons = extButtons,
            .leftX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
            .leftY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
            .rightX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
            .rightY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY),
            .leftTrigger = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT),
            .rightTrigger = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)};
}

bool hasExtendedButtons(SDL_GameController* gc) {
    for (const auto& [button, bit] : kExtendedButtonBits) {
        if (SDL_GameControllerHasButton(gc, button) == SDL_TRUE) { return true; }
    }
    return false;
}

} // namespace dish::input
