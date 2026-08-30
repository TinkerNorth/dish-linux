// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Controller-number allocation, the CONTROLLER_MULTI active mask, and the
// hard-coded capability table a CONTROLLER_ARRIVAL declares — pure logic, so
// every bind/unbind decision is testable without a socket or a pad.
//
// A Moonlight host identifies each virtual pad by a small controller number,
// and every CONTROLLER_MULTI carries a bitfield of the controllers present.
// Clearing a controller's bit while still naming it in `ctrl #` is how the
// protocol signals an unplug, so the last packet after an unbind has to go out
// with the bit already dropped.
//
// THE CAPABILITY TABLE IS CLIENT-SIDE KNOWLEDGE. No Moonlight host exposes what
// its emulated devices can do: /serverinfo carries no controller element,
// /applist rows carry only title/id/HDR, and Wolf's serverinfo() signature has
// no field to put it in. The data flows the other way — the host builds its
// virtual device FROM the type byte and capability bitfield this client
// declares, and never reports the choice back. So the table below is what the
// reference host actually constructs per type (Wolf create_new_joypad), and no
// copy anywhere may promise the host will honour the pick.

#pragma once

#include "core/moonlight/MoonlightProtocol.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace dish::moonlight {

// Four virtual pads per session, the XInput ceiling every reference host
// implements. Named here rather than written at a call site.
inline constexpr std::uint8_t kMaxPads = 4;

// slotId -> controller number, plus the derived active mask. Copyable, so a
// caller can snapshot it under a lock and act on the copy outside.
class PadSlots {
  public:
    // Assigns the lowest free controller number. nullopt when the session is
    // full or the slot already holds one — Wolf skips a CONTROLLER_ARRIVAL for
    // a number already present, so a live index is never reused.
    std::optional<std::uint8_t> assign(const std::string& slotId) {
        if (assigned_.count(slotId) != 0) { return std::nullopt; }
        for (std::uint8_t n = 0; n < kMaxPads; ++n) {
            bool taken = false;
            for (const auto& [id, num] : assigned_) {
                if (num == n) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                assigned_[slotId] = n;
                return n;
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint8_t> numberFor(const std::string& slotId) const {
        const auto it = assigned_.find(slotId);
        if (it == assigned_.end()) { return std::nullopt; }
        return it->second;
    }

    // The slot holding `number`, if any. The host addresses rumble, trigger
    // rumble and RGB by controller number, so this is how an inbound event
    // finds the pad it belongs to.
    std::optional<std::string> slotFor(std::uint8_t number) const {
        for (const auto& [id, num] : assigned_) {
            if (num == number) { return id; }
        }
        return std::nullopt;
    }

    // Releases the slot and returns the number it held, so the caller can send
    // the final bit-cleared CONTROLLER_MULTI for it.
    std::optional<std::uint8_t> release(const std::string& slotId) {
        const auto it = assigned_.find(slotId);
        if (it == assigned_.end()) { return std::nullopt; }
        const std::uint8_t number = it->second;
        assigned_.erase(it);
        return number;
    }

    std::uint16_t activeMask() const {
        std::uint16_t mask = 0;
        for (const auto& [id, num] : assigned_) {
            mask = static_cast<std::uint16_t>(mask | (1U << num));
        }
        return mask;
    }

    bool empty() const { return assigned_.empty(); }
    bool full() const { return assigned_.size() >= kMaxPads; }
    std::size_t size() const { return assigned_.size(); }

    const std::map<std::string, std::uint8_t>& all() const { return assigned_; }

  private:
    std::map<std::string, std::uint8_t> assigned_;
};

// What the local input source can actually deliver. Declaring a capability the
// source cannot provide makes the host ask for motion reports that never
// arrive, so the declared bitfield is always intersected with this.
struct SourceCapabilities {
    bool rumble = false;
    bool motion = false;
    bool touchpad = false;
    bool battery = false;
    bool lightbar = false;
};

inline std::uint8_t sourceCapabilityBits(const SourceCapabilities& source) {
    // Analog triggers are present on every pad Dish forwards.
    std::uint8_t bits = moonproto::kCapAnalogTriggers;
    if (source.rumble) {
        bits =
            static_cast<std::uint8_t>(bits | moonproto::kCapRumble | moonproto::kCapTriggerRumble);
    }
    if (source.motion) {
        bits = static_cast<std::uint8_t>(bits | moonproto::kCapAccelerometer | moonproto::kCapGyro);
    }
    if (source.touchpad) { bits = static_cast<std::uint8_t>(bits | moonproto::kCapTouchpad); }
    if (source.battery) { bits = static_cast<std::uint8_t>(bits | moonproto::kCapBattery); }
    if (source.lightbar) { bits = static_cast<std::uint8_t>(bits | moonproto::kCapRgbLed); }
    return bits;
}

// The most a host's emulated device of this type can carry, whatever the pad
// behind it offers. A PlayStation pad is the only one the reference host wires
// motion, touch, battery and an LED into; its Xbox and Nintendo devices are
// sticks, buttons, analog triggers and body rumble.
//
// Nintendo carries NO MOTION here, unlike the satellite `switchpro` type. The
// two are different type systems that happen to share names: the host requests
// accelerometer and gyro only for a PlayStation device and routes motion only
// into its PS5 joypad.
inline std::uint8_t typeCapabilityCeiling(std::uint8_t controllerType) {
    if (controllerType == moonproto::kControllerTypePs) {
        return static_cast<std::uint8_t>(moonproto::kCapAnalogTriggers | moonproto::kCapRumble |
                                         moonproto::kCapTriggerRumble | moonproto::kCapTouchpad |
                                         moonproto::kCapAccelerometer | moonproto::kCapGyro |
                                         moonproto::kCapBattery | moonproto::kCapRgbLed);
    }
    return static_cast<std::uint8_t>(moonproto::kCapAnalogTriggers | moonproto::kCapRumble);
}

// Auto resolves HERE, on the client, before anything reaches the wire: a source
// that reports gyro or accelerometer becomes PlayStation, everything else Xbox.
// It is the only rule that both matches the reference host's own promotion of
// an UNKNOWN-with-motion pad to PlayStation and lets the type card state what
// the pad will really support.
inline std::uint8_t resolveAutoType(bool sourceHasMotion) {
    return sourceHasMotion ? moonproto::kControllerTypePs : moonproto::kControllerTypeXbox;
}

// A stored pick, normalised. A record written before the sentinel converged
// holds 0 for Auto, which collides with the wire's CONTROLLER_TYPE_UNKNOWN, so
// it migrates on read; so does anything outside the picker's own range.
inline int migrateControllerType(int stored) {
    if (stored == moonproto::kControllerTypeXbox || stored == moonproto::kControllerTypePs ||
        stored == moonproto::kControllerTypeNintendo) {
        return stored;
    }
    return moonproto::kControllerTypeAuto;
}

// The stored pick as a wire type byte, with Auto resolved against the source.
inline std::uint8_t resolveControllerType(int stored, bool sourceHasMotion) {
    const int normalised = migrateControllerType(stored);
    if (normalised == moonproto::kControllerTypeAuto) { return resolveAutoType(sourceHasMotion); }
    return static_cast<std::uint8_t>(normalised);
}

// What CONTROLLER_ARRIVAL declares: the type's ceiling intersected with what
// the source can deliver.
inline std::uint8_t declaredCapabilities(std::uint8_t resolvedType,
                                         const SourceCapabilities& source) {
    return static_cast<std::uint8_t>(typeCapabilityCeiling(resolvedType) &
                                     sourceCapabilityBits(source));
}

// The advertised button set: the whole legacy 16-bit word, plus the touchpad
// click only when a touchpad is in the live set for this binding.
inline std::uint32_t declaredButtons(std::uint8_t declaredCaps) {
    std::uint32_t buttons = moonproto::kStandardButtons;
    if ((declaredCaps & moonproto::kCapTouchpad) != 0) { buttons |= moonproto::kBtnTouchpad; }
    return buttons;
}

} // namespace dish::moonlight
