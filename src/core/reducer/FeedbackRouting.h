// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Where a feedback message for one slot has to go, and whether the descriptor
// may advertise it at all. One owner for both questions, because they are the
// same question: a capability the satellite is told about is a promise that the
// matching dispatch lands somewhere.
//
// Two paths can carry feedback, and they carry different amounts of it:
//
//   Standard  the SDL layer. Rumble and a single lightbar colour, because that
//             is the whole of SDL's output API for a gamepad. There is no
//             adaptive-trigger or player-LED call to make.
//   Direct    a raw-HID claim. Everything the family's OUT report has, built by
//             core/input/UsbOutputReports.h -- but only while the claim is
//             actually live. A slot the user moved back to Standard, or whose
//             claim died, must stop advertising immediately or the satellite
//             keeps sending into a hole.
//
// Deliberately NOT modelled here: a fallback from Direct to Standard. A pad on
// the Direct path is not open on the SDL path at the same time, so "try the
// other one" would be a write to a handle that does not exist.

#pragma once

#include <cstdint>

namespace dish::reducer {

enum class FeedbackKind : std::uint8_t {
    Rumble,
    Lightbar,
    TriggerEffects,
    PlayerLeds,
};

enum class FeedbackTarget : std::uint8_t {
    // Nothing can actuate this: do not advertise, do not dispatch.
    None,
    // The SDL bridge's output queue.
    Standard,
    // The USB-direct claim's OUT report path.
    DirectUsb,
};

// The pad-and-path facts a slot answers with. `pad*` is the hardware ("does
// this model have the surface at all"), independent of which path is bound.
struct SlotFeedbackInputs {
    // The slot is a synthetic (USB-direct) id rather than an SDL one.
    bool usbDirect = false;
    // A Direct claim for the model is live right now. Only meaningful when
    // usbDirect; a synthetic slot whose claim went away can drive nothing.
    bool directClaimLive = false;

    bool padRumble = false;
    bool padLightbar = false;
    bool padTriggerEffects = false;
    bool padPlayerLeds = false;
};

namespace detail {

inline bool padHas(const SlotFeedbackInputs& in, FeedbackKind kind) {
    switch (kind) {
    case FeedbackKind::Rumble:
        return in.padRumble;
    case FeedbackKind::Lightbar:
        return in.padLightbar;
    case FeedbackKind::TriggerEffects:
        return in.padTriggerEffects;
    case FeedbackKind::PlayerLeds:
        return in.padPlayerLeds;
    }
    return false;
}

// What the SDL layer can drive, whatever the pad has.
inline bool standardPathCarries(FeedbackKind kind) {
    return kind == FeedbackKind::Rumble || kind == FeedbackKind::Lightbar;
}

} // namespace detail

// The target for a dispatch, and the same answer the descriptor's caps use:
// None means "do not advertise".
inline FeedbackTarget resolveFeedbackTarget(const SlotFeedbackInputs& in, FeedbackKind kind) {
    if (!detail::padHas(in, kind)) { return FeedbackTarget::None; }
    if (in.usbDirect) {
        return in.directClaimLive ? FeedbackTarget::DirectUsb : FeedbackTarget::None;
    }
    return detail::standardPathCarries(kind) ? FeedbackTarget::Standard : FeedbackTarget::None;
}

// Whether the descriptor may claim this actuator. Exactly "some path carries
// it", so a cap can never outlive the dispatch that would honour it.
inline bool slotCarriesFeedback(const SlotFeedbackInputs& in, FeedbackKind kind) {
    return resolveFeedbackTarget(in, kind) != FeedbackTarget::None;
}

} // namespace dish::reducer
