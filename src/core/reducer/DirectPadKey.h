// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One pad on the USB-direct path, and the slot id its card, its binding and its input slot go by.
// A model's pad is the model, and keeps the decimal model key (slotPathVpKey) it always had. A
// wireless receiver serves up to four paired controllers under one vid:pid, so each of its pads is
// also the HID interface its slot rides, written after a colon. The interface, never the order the
// controllers turned up in, is what brings a controller that reconnects back to its own card.
// Two devices of one model (two identical pads, two receivers) are units of it: the first keeps
// the ids above, and a later one writes its unit after the model key.

#pragma once

#include "core/reducer/SlotPathFields.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace dish::reducer {

inline constexpr int kNoReceiverSlot = -1;
inline constexpr char kReceiverSlotSeparator = ':';
// An interface number is one byte.
inline constexpr std::size_t kMaxReceiverSlotDigits = 3;
inline constexpr int kMaxReceiverSlot = 255;
inline constexpr char kDeviceUnitSeparator = '#';

struct DirectPadKey {
    int vendorId = 0;
    int productId = 0;
    // The receiver interface the pad rides, or kNoReceiverSlot for a model's own pad.
    int slot = kNoReceiverSlot;
    // Which of the model's devices the pad rides, 0 for the first.
    int unit = 0;

    bool operator==(const DirectPadKey& o) const {
        return vendorId == o.vendorId && productId == o.productId && slot == o.slot &&
               unit == o.unit;
    }
    bool operator!=(const DirectPadKey& o) const { return !(*this == o); }
    // In the order the model keys always listed the pads, a model's devices one after the other
    // and a receiver's slots in turn.
    bool operator<(const DirectPadKey& o) const {
        const int model = slotPathVpKey(vendorId, productId);
        const int otherModel = slotPathVpKey(o.vendorId, o.productId);
        if (model != otherModel) { return model < otherModel; }
        if (unit != o.unit) { return unit < o.unit; }
        return slot < o.slot;
    }
};

inline bool isReceiverSlot(const DirectPadKey& pad) { return pad.slot != kNoReceiverSlot; }

// The device a pad rides: a model's own pad is its device, a receiver's slot rides the receiver.
inline DirectPadKey deviceKeyOf(const DirectPadKey& pad) {
    return DirectPadKey{pad.vendorId, pad.productId, kNoReceiverSlot, pad.unit};
}

inline bool isPadOfModel(const DirectPadKey& pad, int vendorId, int productId) {
    return pad.vendorId == vendorId && pad.productId == productId;
}

inline std::string modelSlotId(const DirectPadKey& pad) {
    return std::to_string(slotPathVpKey(pad.vendorId, pad.productId));
}

inline std::string deviceSlotId(const DirectPadKey& pad) {
    if (pad.unit == 0) { return modelSlotId(pad); }
    return modelSlotId(pad) + kDeviceUnitSeparator + std::to_string(pad.unit);
}

inline std::string directSlotId(const DirectPadKey& pad) {
    if (!isReceiverSlot(pad)) { return deviceSlotId(pad); }
    return deviceSlotId(pad) + kReceiverSlotSeparator + std::to_string(pad.slot);
}

namespace detail {

// A receiver's interface or a model's unit: a byte, in decimal digits and nothing else.
inline std::optional<int> parseSlotByte(const std::string& digits) {
    const bool fits = !digits.empty() && digits.size() <= kMaxReceiverSlotDigits;
    if (!fits) { return std::nullopt; }
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') { return std::nullopt; }
    }
    const int value = std::stoi(digits);
    if (value > kMaxReceiverSlot) { return std::nullopt; }
    return value;
}

} // namespace detail

// The inverse of deviceSlotId.
inline std::optional<DirectPadKey> parseDeviceSlotId(const std::string& id) {
    const std::size_t separator = id.find(kDeviceUnitSeparator);
    const std::optional<std::pair<int, int>> model = parseSyntheticSlotId(id.substr(0, separator));
    if (!model.has_value()) { return std::nullopt; }
    if (separator == std::string::npos) { return DirectPadKey{model->first, model->second}; }
    const std::optional<int> unit = detail::parseSlotByte(id.substr(separator + 1));
    const bool isALaterDevice = unit.has_value() && *unit > 0;
    if (!isALaterDevice) { return std::nullopt; }
    return DirectPadKey{model->first, model->second, kNoReceiverSlot, *unit};
}

// The inverse of directSlotId. An SDL slot id ("sdl:3") never parses: its model part is not a
// decimal model key.
inline std::optional<DirectPadKey> parseDirectSlotId(const std::string& id) {
    const std::size_t separator = id.find(kReceiverSlotSeparator);
    const std::optional<DirectPadKey> device = parseDeviceSlotId(id.substr(0, separator));
    if (!device.has_value()) { return std::nullopt; }
    if (separator == std::string::npos) { return device; }
    const std::optional<int> slot = detail::parseSlotByte(id.substr(separator + 1));
    if (!slot.has_value()) { return std::nullopt; }
    return DirectPadKey{device->vendorId, device->productId, *slot, device->unit};
}

// A receiver's pad is one controller of several under its model, so its slot id stands only for
// itself: the binding flow never re-resolves it through the model onto a sibling slot's pad, the
// way an SDL slot or a model's own pad follows its model across a path switch.
inline bool isReceiverSlotId(const std::string& id) {
    const std::optional<DirectPadKey> pad = parseDirectSlotId(id);
    return pad.has_value() && isReceiverSlot(*pad);
}

// What a binding left on a departed Direct slot still names for the binding-presence gate
// (BindingPresence.h): the model of a model's own pad, so a path switch can carry the binding to
// the pad's new slot id. Nothing for a receiver's slot, whose binding leaves with its controller
// and never moves onto a slot that carries someone else's.
inline std::optional<std::pair<int, int>> departedPadIdentity(const DirectPadKey& pad) {
    if (isReceiverSlot(pad)) { return std::nullopt; }
    return std::make_pair(pad.vendorId, pad.productId);
}

} // namespace dish::reducer
