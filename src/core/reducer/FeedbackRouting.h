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
//   Standard  the SDL layer. Rumble and a single lightbar colour, which is the
//             whole of SDL's typed output API for a gamepad -- plus
//             SDL_GameControllerSendEffect, a raw effect body that only SDL's
//             own HIDAPI drivers accept. For a DualSense that body is its OUT
//             report minus the id, so a HIDAPI-owned DualSense takes the
//             adaptive triggers, player LEDs and mic lamp too, on USB or
//             Bluetooth alike; `standardEffects` says whether this pad's
//             driver is that one. Every other SDL backend (XInput,
//             DirectInput, evdev) refuses the call, and for every other family
//             the body would be the wrong report.
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
    // The mic-mute lamp (kMsgMicLed). A feedback kind like the others even
    // though its gate is CAP_MIC rather than a cap of its own: the routing
    // question — where does a delivered state land — is the same question.
    MicLed,
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
    // The model has a mic-mute lamp (DualSense only).
    bool padMicLed = false;

    // The SDL layer takes this pad's raw effect body: SDL's own HIDAPI driver
    // owns it, so SDL_GameControllerSendEffect lands, and the body it takes is
    // the family's OUT report that core/input/UsbOutputReports.h builds. Only
    // meaningful when !usbDirect; a Direct claim writes the report itself.
    bool standardEffects = false;

    // Controller-audio routes: does the pad have a usable audio path on THIS
    // machine (its own USB-audio endpoints matched to an audio device)?
    // Deliberately independent of the HID path above — the pad's audio
    // function is a separate USB interface, reachable whether input rides
    // Standard or Direct — and false for a Bluetooth pad, which has none.
    bool padMicRoute = false;
    bool padSpeakerRoute = false;
    // Protocol 3: the speaker route's endpoint is the DualSense's 4-channel
    // one, so the haptic lanes have somewhere to play. Never true without
    // padSpeakerRoute; the matcher sets them together.
    bool padHapticRoute = false;
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
    case FeedbackKind::MicLed:
        return in.padMicLed;
    }
    return false;
}

// What the SDL layer can drive, whatever the pad has. Rumble and the lightbar
// always: they are SDL's own calls. The triggers, player LEDs and mic lamp only
// through SendEffect, so only where SDL's HIDAPI driver has the pad; the bytes
// are the same OUT report the Direct path writes, from the same builders,
// handed to SDL instead of the device.
inline bool standardPathCarries(const SlotFeedbackInputs& in, FeedbackKind kind) {
    switch (kind) {
    case FeedbackKind::Rumble:
    case FeedbackKind::Lightbar:
        return true;
    case FeedbackKind::TriggerEffects:
    case FeedbackKind::PlayerLeds:
    case FeedbackKind::MicLed:
        return in.standardEffects;
    }
    return false;
}

} // namespace detail

// The target for a dispatch, and the same answer the descriptor's caps use:
// None means "do not advertise".
inline FeedbackTarget resolveFeedbackTarget(const SlotFeedbackInputs& in, FeedbackKind kind) {
    if (!detail::padHas(in, kind)) { return FeedbackTarget::None; }
    if (in.usbDirect) {
        return in.directClaimLive ? FeedbackTarget::DirectUsb : FeedbackTarget::None;
    }
    return detail::standardPathCarries(in, kind) ? FeedbackTarget::Standard : FeedbackTarget::None;
}

// Whether the descriptor may claim this actuator. Exactly "some path carries
// it", so a cap can never outlive the dispatch that would honour it.
inline bool slotCarriesFeedback(const SlotFeedbackInputs& in, FeedbackKind kind) {
    return resolveFeedbackTarget(in, kind) != FeedbackTarget::None;
}

// Whether the descriptor may claim the controller-audio caps. The same rule as
// the feedback kinds — a cap is a promise something lands — but keyed on the
// audio routes rather than the HID path, because the streams ride the pad's own
// audio endpoints (see SlotFeedbackInputs::padMicRoute). CAP_MIC also gates the
// kMsgMicLed return path server-side, so the lamp needs no claim of its own.
inline bool slotCarriesMicCapture(const SlotFeedbackInputs& in) { return in.padMicRoute; }

inline bool slotCarriesSpeakerPlayout(const SlotFeedbackInputs& in) { return in.padSpeakerRoute; }

// kCapHapticAudio is the promise the WAVEFORM lands: only a slot whose named
// endpoint carries the actuator lanes may claim it. Without the claim the host
// reduces the lanes to rumble for this slot, which is the right rendering for
// every other pad and is why a false here is a fallback rather than a loss.
inline bool slotCarriesHapticPlayout(const SlotFeedbackInputs& in) {
    return in.padSpeakerRoute && in.padHapticRoute;
}

} // namespace dish::reducer
