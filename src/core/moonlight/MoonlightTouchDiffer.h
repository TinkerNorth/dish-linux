// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Turns the pad's FULL-STATE touch frame into the EVENT stream a Moonlight host
// expects.
//
// The two protocols disagree about what a touch sample is, and that disagreement
// is the whole reason this file exists. A DualShock 4 / DualSense report carries
// "here is where both fingers are right now", and the satellite forwards exactly
// that, loss-safely, every frame. CONTROLLER_TOUCH instead carries "this pointer
// went down / moved / came up", and the host holds the contact open until an UP
// arrives. Feeding one into the other unchanged would either strand a contact
// forever (no UP is ever generated) or re-DOWN it on every report.
//
// So the differ owns the last frame and emits only transitions. It is a pure
// value type: one instance per (session, controller), reset when the pad or the
// session goes away, and every rule below is directly testable.
//
// Loss: the satellite path can drop a frame safely because the next one carries
// the whole truth. This path cannot, which is fine, because the Moonlight
// control stream is reliable (ENet, ordered). A dropped frame here would strand
// a contact, so the events must not ride an unreliable channel.

#pragma once

#include "core/moonlight/MoonlightProtocol.h"

#include <cstdint>
#include <vector>

namespace dish::moonlight {

// One CONTROLLER_TOUCH event, ready for the encoder. Coordinates are already
// normalised 0..1.
struct TouchEvent {
    std::uint8_t eventType = moonproto::kTouchEventDown;
    std::uint32_t pointerId = 0;
    float x = 0.0F;
    float y = 0.0F;
    float pressure = 0.0F;

    bool operator==(const TouchEvent& o) const {
        return eventType == o.eventType && pointerId == o.pointerId && x == o.x && y == o.y &&
               pressure == o.pressure;
    }
};

// One finger's slot in the pad's frame.
struct TouchFinger {
    bool active = false;
    std::uint8_t id = 0;
    float x = 0.0F;
    float y = 0.0F;

    bool operator==(const TouchFinger& o) const {
        return active == o.active && id == o.id && x == o.x && y == o.y;
    }
};

// Pressure is not measured by any pad here: the report says contact or no
// contact. 1.0 is a solid contact, and an UP carries 0.0 because the host reads
// the pressure of the releasing event.
inline constexpr float kTouchPressureDown = 1.0F;
inline constexpr float kTouchPressureUp = 0.0F;

class MoonlightTouchDiffer {
  public:
    // Drop the remembered frame. Call when the pad unbinds or the session ends,
    // so a returning pad starts from "nothing is touching" rather than emitting
    // a MOVE from a contact the host no longer holds.
    void reset() {
        last_[0] = TouchFinger{};
        last_[1] = TouchFinger{};
    }

    // The transitions between the remembered frame and this one, in slot order.
    // Empty when nothing changed, which is the common case: a pad streams its
    // touch block every report whether or not a finger moved.
    std::vector<TouchEvent> diff(const TouchFinger& finger0, const TouchFinger& finger1) {
        std::vector<TouchEvent> out;
        out.reserve(4);
        diffFinger(last_[0], finger0, out);
        diffFinger(last_[1], finger1, out);
        last_[0] = finger0;
        last_[1] = finger1;
        return out;
    }

  private:
    // A tracking id change with no gap between contacts is a NEW finger: the pad
    // renumbers on every fresh touch, and the host would otherwise carry the old
    // pointer's identity into a contact that is not it. Closing the old one
    // first is what keeps the host's pointer set from leaking.
    static void diffFinger(const TouchFinger& prev, const TouchFinger& cur,
                           std::vector<TouchEvent>& out) {
        if (!prev.active && cur.active) {
            out.push_back(
                TouchEvent{moonproto::kTouchEventDown, cur.id, cur.x, cur.y, kTouchPressureDown});
            return;
        }
        if (prev.active && !cur.active) {
            out.push_back(
                TouchEvent{moonproto::kTouchEventUp, prev.id, prev.x, prev.y, kTouchPressureUp});
            return;
        }
        if (!prev.active && !cur.active) { return; }
        if (prev.id != cur.id) {
            out.push_back(
                TouchEvent{moonproto::kTouchEventUp, prev.id, prev.x, prev.y, kTouchPressureUp});
            out.push_back(
                TouchEvent{moonproto::kTouchEventDown, cur.id, cur.x, cur.y, kTouchPressureDown});
            return;
        }
        // Position-only: a MOVE for the same pointer. Deliberately exact
        // comparison rather than an epsilon — the coordinates come from integer
        // hardware counts, so "unchanged" is exact, and a threshold here would
        // silently swallow slow drags.
        if (prev.x != cur.x || prev.y != cur.y) {
            out.push_back(
                TouchEvent{moonproto::kTouchEventMove, cur.id, cur.x, cur.y, kTouchPressureDown});
        }
    }

    TouchFinger last_[2];
};

} // namespace dish::moonlight
