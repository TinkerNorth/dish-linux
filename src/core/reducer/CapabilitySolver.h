// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The single owner of the capability vocabulary shared by the wizard's type table
// and Configure binding's WHAT CARRIES matrix, so the two surfaces cannot drift.
//
//     available = input n link n type n host
//
//   Input  what the pad itself reports
//   Link   the USB path. Standard (SDL) carries everything the pad's driver
//          exposes — the input layer's per-pad probe is what constrains it —
//          but SDL has no adaptive-trigger or player-LED call, so those two
//          never fire there. Direct reads everything the pad sends AND writes
//          its OUT reports, so it carries every actuator its family has.
//          Mirrors dish-android's per-path rule: a capability shows only where
//          it fires.
//   Type   the catalog type's features: an Xbox 360 type carries no gyro however
//          good the pad is
//   Host   the satellite's hostFeatures. A Bluetooth host is the system gamepad
//          layer and carries only the gamepad, its triggers and rumble
//
// Off means every layer carries it and the user switched it off, which is why it
// is distinct from Unavailable. Pending means the host or catalog has not
// resolved; an unread layer never refuses, so a cross is never drawn from a
// guess. Returns tokens only; localized copy lives in QML.

#pragma once

#include <vector>
#include <cstdint>

namespace dish::reducer {

enum class CapLayer : std::uint8_t { Input, Link, Type, Host };
enum class CapVerdict : std::uint8_t { Available, Unavailable, Pending, Off };

// Declaration order is the render order both surfaces use.
enum class CapFeature : std::uint8_t {
    Gamepad,
    Triggers,
    Motion,
    Touchpad,
    Mouse,
    Rumble,
    Lightbar,
    TriggerEffects,
    PlayerLeds,
    Mic,
    Speaker
};

struct CapabilityInputs {
    bool padMotion = false;
    bool padTouchpad = false;
    // The pad's motors (the SDL probe for a framework slot, the parser family
    // for a synthetic). Defaults true so an unknown slot doesn't refuse at the
    // input layer; the link layer still gates the path.
    bool padRumble = true;
    bool padLightbar = false;
    // Protocol-2 actuators. Default false: unlike rumble these are rare enough
    // that an unknown pad claiming them would be the surprising answer.
    bool padTriggerEffects = false;
    bool padPlayerLeds = false;
    // Controller audio: does the pad have a usable audio route on this machine
    // (core/reducer/FeedbackRouting.h answers it). Default false for the same
    // reason as the two above — and false everywhere until Wave 2 lands the
    // route matching, so no binding shows audio as carried yet.
    bool padMic = false;
    bool padSpeaker = false;

    bool linkDirect = false;   // usb && directCapable && the draft wants Direct
    bool linkUsb = false;      // false means Bluetooth transport
    bool padClaimable = false; // pathSupported

    bool typeResolved = false; // false means the type layer refuses nothing
    bool typeMotion = false, typeTouchpad = false, typeRumble = false, typeLightbar = false;
    bool typeTriggerEffects = false, typePlayerLeds = false;
    // From the catalog's mic/speaker feature slugs: true only for the types
    // whose materializer can build a pad with audio endpoints (the two Sony
    // types via composite personas).
    bool typeMic = false, typeSpeaker = false;

    bool hostResolved = false; // false means Pending
    bool hostIsBluetooth = false;
    bool hostMouseControl = false;
    bool hostRumble = false;
    // The controller-audio probe's per-direction verdict (reducer/
    // HostAudioVerdict.h), NOT the catalog: the catalog says what the host
    // could build, this says what it will carry right now, and it can flip
    // between sessions. Unlike the two layers above, an unprobed host reads
    // FALSE rather than Pending — audio is opt-in by design, so the honest
    // answer before a probe is "not carried".
    bool hostMic = false, hostSpeaker = false;

    // User gates drive Off, never Unavailable.
    bool userMotionOn = true, userRumbleOn = true;
    // The audio defaults mirror the stores': mic off until asked (privacy),
    // speaker on.
    bool userMicOn = false, userSpeakerOn = true;
    int userTouchpadMode = 0; // 0=off 1=pad 2=mouse
};

struct CapabilityRow {
    CapFeature feature{};
    bool inOk = false, linkOk = false, typeOk = false, hostOk = false;
    CapVerdict verdict = CapVerdict::Pending;
    CapLayer failingLayer = CapLayer::Input; // meaningful iff verdict == Unavailable
    bool hasFailingLayer = false;
};

namespace detail {

// `linkUsb` and `padClaimable` never change the carried set, only `linkDirect`
// does; they are carried so the caller can pick the right reason to show.
inline bool inputCarries(const CapabilityInputs& in, CapFeature f) {
    switch (f) {
    case CapFeature::Gamepad:
    case CapFeature::Triggers:
        return true;
    case CapFeature::Motion:
        return in.padMotion;
    // Touchpad and Mouse deliberately share one answer: Mouse is a routing OF
    // the touchpad, so the pad needs one to drive it. They stay separate cases
    // because every other layer treats them as distinct features.
    // NOLINTBEGIN(bugprone-branch-clone)
    case CapFeature::Touchpad:
        return in.padTouchpad;
    case CapFeature::Mouse:
        return in.padTouchpad;
    // NOLINTEND(bugprone-branch-clone)
    case CapFeature::Rumble:
        return in.padRumble;
    case CapFeature::Lightbar:
        return in.padLightbar;
    case CapFeature::TriggerEffects:
        return in.padTriggerEffects;
    case CapFeature::PlayerLeds:
        return in.padPlayerLeds;
    case CapFeature::Mic:
        return in.padMic;
    case CapFeature::Speaker:
        return in.padSpeaker;
    }
    return false;
}

inline bool linkCarries(const CapabilityInputs& in, CapFeature f) {
    if (in.linkDirect) {
        // A raw-HID claim both reads the pad's IN reports and writes its OUT
        // ones, so every actuator the family has is reachable.
        return true;
    }
    // Standard (SDL) forwards motion, touch, rumble and the lightbar wherever
    // the pad's driver exposes them; the input layer's probe constrains it. The
    // adaptive triggers and the player LEDs have no SDL call at all, so they
    // are the one thing the Standard path structurally cannot carry. Audio is
    // NOT with them: the streams ride the pad's own USB-audio endpoints, a
    // separate interface reachable from either HID path, so the link layer
    // never refuses it — the input layer's route facts are what constrain it.
    return f != CapFeature::TriggerEffects && f != CapFeature::PlayerLeds;
}

inline bool typeCarries(const CapabilityInputs& in, CapFeature f) {
    // An unread catalog refuses nothing: a guessed "unsupported" is worse than no
    // table, and the Pending verdict is what says we do not know yet.
    if (!in.typeResolved) { return true; }
    switch (f) {
    case CapFeature::Gamepad:
    case CapFeature::Triggers:
        return true;
    case CapFeature::Motion:
        return in.typeMotion;
    case CapFeature::Touchpad:
        return in.typeTouchpad;
    case CapFeature::Mouse: // a routing, not a catalog type feature
        return true;
    case CapFeature::Rumble:
        return in.typeRumble;
    case CapFeature::Lightbar:
        return in.typeLightbar;
    case CapFeature::TriggerEffects:
        return in.typeTriggerEffects;
    case CapFeature::PlayerLeds:
        return in.typePlayerLeds;
    case CapFeature::Mic:
        return in.typeMic;
    case CapFeature::Speaker:
        return in.typeSpeaker;
    }
    return false;
}

inline bool hostCarries(const CapabilityInputs& in, CapFeature f) {
    if (!in.hostResolved) { return true; } // same rule as the type layer
    if (in.hostIsBluetooth) {
        // The system gamepad layer has no motion, touch, lightbar, trigger
        // effect, player-LED or controller-audio channel.
        return f == CapFeature::Gamepad || f == CapFeature::Triggers || f == CapFeature::Rumble;
    }
    switch (f) {
    case CapFeature::Gamepad:
    case CapFeature::Triggers:
    case CapFeature::Motion:
    case CapFeature::Touchpad:
    case CapFeature::Lightbar:
    case CapFeature::TriggerEffects:
    case CapFeature::PlayerLeds:
        return true;
    case CapFeature::Mouse:
        return in.hostMouseControl;
    case CapFeature::Rumble:
        return in.hostRumble;
    // The probe's live verdict, conservative-false until it lands: an
    // unprobed host is Unavailable-at-host here rather than Pending, because
    // no other layer can promise the host will carry the stream.
    case CapFeature::Mic:
        return in.hostMic;
    case CapFeature::Speaker:
        return in.hostSpeaker;
    }
    return false;
}

// Only reached once all four layers carry the feature, so a user switch can never
// masquerade as unsupported.
inline bool userEnabled(const CapabilityInputs& in, CapFeature f) {
    switch (f) {
    case CapFeature::Motion:
        return in.userMotionOn;
    case CapFeature::Rumble:
        return in.userRumbleOn;
    case CapFeature::Touchpad:
        return in.userTouchpadMode == 1;
    case CapFeature::Mouse:
        return in.userTouchpadMode == 2;
    case CapFeature::Mic:
        return in.userMicOn;
    case CapFeature::Speaker:
        return in.userSpeakerOn;
    default:
        return true;
    }
}

} // namespace detail

// A Bluetooth host is not a satellite and offers no types, so only a satellite
// host waits on a catalog.
inline bool capabilitiesPending(const CapabilityInputs& in) {
    return !in.hostResolved || (!in.hostIsBluetooth && !in.typeResolved);
}

inline std::vector<CapabilityRow> solveCapabilities(const CapabilityInputs& in) {
    static constexpr CapFeature kOrder[] = {
        CapFeature::Gamepad,  CapFeature::Triggers,       CapFeature::Motion,
        CapFeature::Touchpad, CapFeature::Mouse,          CapFeature::Rumble,
        CapFeature::Lightbar, CapFeature::TriggerEffects, CapFeature::PlayerLeds,
        CapFeature::Mic,      CapFeature::Speaker};
    const bool pending = capabilitiesPending(in);

    std::vector<CapabilityRow> rows;
    rows.reserve(sizeof(kOrder) / sizeof(kOrder[0]));
    for (const CapFeature f : kOrder) {
        CapabilityRow row;
        row.feature = f;
        row.inOk = detail::inputCarries(in, f);
        row.linkOk = detail::linkCarries(in, f);
        row.typeOk = detail::typeCarries(in, f);
        row.hostOk = detail::hostCarries(in, f);

        if (pending) {
            row.verdict = CapVerdict::Pending;
        } else if (row.inOk && row.linkOk && row.typeOk && row.hostOk) {
            row.verdict = detail::userEnabled(in, f) ? CapVerdict::Available : CapVerdict::Off;
        } else {
            row.verdict = CapVerdict::Unavailable;
            row.hasFailingLayer = true;
            // The first failing layer in Input, Link, Type, Host order: the one
            // whose fix the user can actually act on.
            row.failingLayer = !row.inOk     ? CapLayer::Input
                               : !row.linkOk ? CapLayer::Link
                               : !row.typeOk ? CapLayer::Type
                                             : CapLayer::Host;
        }
        rows.push_back(row);
    }
    return rows;
}

} // namespace dish::reducer
