// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Maps a controller slot's (vid, pid) against the live USB path FSM state to the
// per-slot path fields the Controllers page renders. A slot with no matching
// controller (an Xbox pad the raw-HID gateway never enumerates, or a 0/0
// identity) is unsupported, and the page hides the path control entirely.

#pragma once

#include "core/reducer/PathChoice.h"
#include "core/reducer/UsbPathMachine.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace dish::reducer {

// A plain value type so a test can assert the mapping with no Qt or model type.
struct SlotPathFields {
    UsbPhase phase = UsbPhase::Routed;
    PathChoice desired = PathChoice::Standard;
    bool supported = false; // a Standard/Direct switch is meaningful at all
    std::optional<DirectClaimFailure> failure;

    bool operator==(const SlotPathFields& o) const {
        return phase == o.phase && desired == o.desired && supported == o.supported &&
               failure == o.failure;
    }
    bool operator!=(const SlotPathFields& o) const { return !(*this == o); }
};

// Must stay identical to UsbGamepadManager::vpKey; it keys the same map.
inline int slotPathVpKey(int vendorId, int productId) {
    return (vendorId << 16) | (productId & 0xFFFF);
}

// A vid or pid of 0 short-circuits to unsupported, so an identity-less SDL slot
// never spuriously pairs with the 0/0 key. A Bluetooth slot is unsupported by
// rule, not by lookup: the same model can be present over BOTH transports at
// once (charging over USB while streaming over BT), the controllers map is
// keyed by (vid, pid), and the raw-HID claim can never serve the wireless
// link — so a BT slot must not wear its USB twin's path control.
inline SlotPathFields slotPathFields(int vendorId, int productId, bool bluetooth,
                                     const std::map<int, UsbController>& controllers) {
    SlotPathFields out;
    if (bluetooth || vendorId == 0 || productId == 0) { return out; }
    const auto it = controllers.find(slotPathVpKey(vendorId, productId));
    if (it == controllers.end()) { return out; }
    const UsbController& c = it->second;
    out.supported = true;
    out.phase = c.phase;
    out.desired = c.desired;
    out.failure = c.failure;
    return out;
}

// The single derived "loading" state the toggle binds to: true from a pick until
// the slot is stable on the desired path. Direct additionally waits for a
// measured poll rate, because the claim is near-instant but the completion-rate
// sampler needs a window. A terminal failure, RestoreStuck or NeedsReplug is not
// switching: those surface an error note rather than a perpetual spinner.
inline bool slotPathSwitching(UsbPhase phase, PathChoice desired, bool usbDirect, int directPollHz,
                              bool hasFailure) {
    if (phase == UsbPhase::RestoreStuck || phase == UsbPhase::NeedsReplug) { return false; }
    if (hasFailure) { return false; }
    if (phase == UsbPhase::Claiming || phase == UsbPhase::AwaitingFramework) { return true; }
    if (desired == PathChoice::Direct) { return !usbDirect || directPollHz <= 0; }
    return usbDirect; // desired Standard: still switching while on the synthetic.
}

// The inverse of slotPathVpKey over the decimal slot id AppModel gives a
// synthetic. An SDL slot id such as "sdl:3" never parses here.
inline std::optional<std::pair<int, int>> parseSyntheticSlotId(const std::string& id) {
    if (id.empty()) { return std::nullopt; }
    for (char ch : id) {
        if (ch < '0' || ch > '9') { return std::nullopt; }
    }
    // std::stoll throws on a long all-digit string, and the keys are a 32-bit
    // packed int, so bound the width before converting.
    if (id.size() > 10) { return std::nullopt; }
    const long long key = std::stoll(id);
    if (key < 0 || key > 0xFFFFFFFFLL) { return std::nullopt; }
    const int vendorId = static_cast<int>((key >> 16) & 0xFFFF);
    const int productId = static_cast<int>(key & 0xFFFF);
    return std::make_pair(vendorId, productId);
}

} // namespace dish::reducer
