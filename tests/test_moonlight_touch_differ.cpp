// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Full-state pad frames in, CONTROLLER_TOUCH transitions out.
//
// The failure this guards against is a stranded contact: the host holds a
// pointer open until an UP arrives, so any path that ends a touch without
// emitting one leaves the host believing a finger is still down forever.

#include "core/moonlight/MoonlightTouchDiffer.h"

#include <catch2/catch_test_macros.hpp>

using dish::moonlight::kTouchPressureDown;
using dish::moonlight::kTouchPressureUp;
using dish::moonlight::MoonlightTouchDiffer;
using dish::moonlight::TouchFinger;
using dish::moonproto::kTouchEventDown;
using dish::moonproto::kTouchEventMove;
using dish::moonproto::kTouchEventUp;

namespace {

TouchFinger down(std::uint8_t id, float x, float y) { return TouchFinger{true, id, x, y}; }
const TouchFinger kUp{};

} // namespace

TEST_CASE("an idle pad emits nothing", "[moonlight][touch]") {
    // A pad streams its touch block on every report whether or not anything is
    // happening, so silence has to be the common case or the control stream
    // floods.
    MoonlightTouchDiffer differ;
    CHECK(differ.diff(kUp, kUp).empty());
    CHECK(differ.diff(kUp, kUp).empty());
}

TEST_CASE("a first contact is a DOWN at full pressure", "[moonlight][touch]") {
    MoonlightTouchDiffer differ;
    const auto events = differ.diff(down(7, 0.25F, 0.75F), kUp);
    REQUIRE(events.size() == 1);
    CHECK(events[0].eventType == kTouchEventDown);
    CHECK(events[0].pointerId == 7);
    CHECK(events[0].x == 0.25F);
    CHECK(events[0].y == 0.75F);
    CHECK(events[0].pressure == kTouchPressureDown);
}

TEST_CASE("an unchanged contact emits nothing", "[moonlight][touch]") {
    // Re-emitting DOWN or MOVE for a finger resting still is the other flood.
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(7, 0.5F, 0.5F), kUp).size() == 1);
    CHECK(differ.diff(down(7, 0.5F, 0.5F), kUp).empty());
    CHECK(differ.diff(down(7, 0.5F, 0.5F), kUp).empty());
}

TEST_CASE("a moved contact is a MOVE for the same pointer", "[moonlight][touch]") {
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(7, 0.5F, 0.5F), kUp).size() == 1);
    const auto events = differ.diff(down(7, 0.6F, 0.5F), kUp);
    REQUIRE(events.size() == 1);
    CHECK(events[0].eventType == kTouchEventMove);
    CHECK(events[0].pointerId == 7);
    CHECK(events[0].x == 0.6F);
    CHECK(events[0].pressure == kTouchPressureDown);
}

TEST_CASE("a movement on either axis alone still emits a MOVE", "[moonlight][touch]") {
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(1, 0.5F, 0.5F), kUp).size() == 1);
    CHECK(differ.diff(down(1, 0.5F, 0.51F), kUp).size() == 1);
    CHECK(differ.diff(down(1, 0.51F, 0.51F), kUp).size() == 1);
}

TEST_CASE("a lifted contact is an UP at the last known position", "[moonlight][touch]") {
    // The released frame carries no position, so the UP has to report where the
    // finger WAS. Reporting the empty slot's zeros would jump the host's pointer
    // to the pad's corner on every release.
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(3, 0.8F, 0.2F), kUp).size() == 1);
    const auto events = differ.diff(kUp, kUp);
    REQUIRE(events.size() == 1);
    CHECK(events[0].eventType == kTouchEventUp);
    CHECK(events[0].pointerId == 3);
    CHECK(events[0].x == 0.8F);
    CHECK(events[0].y == 0.2F);
    CHECK(events[0].pressure == kTouchPressureUp);
}

TEST_CASE("a tracking-id change closes the old pointer before opening the new one",
          "[moonlight][touch]") {
    // The pad renumbers on every fresh touch. Two contacts back to back inside
    // one frame gap must not be reported as the first one teleporting, or the
    // host's pointer set leaks an id that is never closed.
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(4, 0.1F, 0.1F), kUp).size() == 1);
    const auto events = differ.diff(down(5, 0.9F, 0.9F), kUp);
    REQUIRE(events.size() == 2);
    CHECK(events[0].eventType == kTouchEventUp);
    CHECK(events[0].pointerId == 4);
    CHECK(events[0].x == 0.1F); // the OLD position, not the new one
    CHECK(events[1].eventType == kTouchEventDown);
    CHECK(events[1].pointerId == 5);
    CHECK(events[1].x == 0.9F);
}

TEST_CASE("the two fingers are tracked independently", "[moonlight][touch]") {
    MoonlightTouchDiffer differ;
    // Finger 0 goes down alone.
    REQUIRE(differ.diff(down(1, 0.2F, 0.2F), kUp).size() == 1);
    // Finger 1 joins: only its DOWN, because finger 0 did not move.
    auto events = differ.diff(down(1, 0.2F, 0.2F), down(2, 0.8F, 0.8F));
    REQUIRE(events.size() == 1);
    CHECK(events[0].eventType == kTouchEventDown);
    CHECK(events[0].pointerId == 2);
    // Finger 0 lifts while finger 1 moves: one UP, one MOVE, in slot order.
    events = differ.diff(kUp, down(2, 0.7F, 0.8F));
    REQUIRE(events.size() == 2);
    CHECK(events[0].eventType == kTouchEventUp);
    CHECK(events[0].pointerId == 1);
    CHECK(events[1].eventType == kTouchEventMove);
    CHECK(events[1].pointerId == 2);
}

TEST_CASE("both fingers lifting at once closes both", "[moonlight][touch]") {
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(1, 0.2F, 0.2F), down(2, 0.8F, 0.8F)).size() == 2);
    const auto events = differ.diff(kUp, kUp);
    REQUIRE(events.size() == 2);
    CHECK(events[0].eventType == kTouchEventUp);
    CHECK(events[0].pointerId == 1);
    CHECK(events[1].eventType == kTouchEventUp);
    CHECK(events[1].pointerId == 2);
}

TEST_CASE("the same id in both slots stays two pointers", "[moonlight][touch]") {
    // Slot identity, not id identity, is what the differ tracks. A pad that
    // reused an id across slots must still produce two contacts.
    MoonlightTouchDiffer differ;
    const auto events = differ.diff(down(9, 0.1F, 0.1F), down(9, 0.9F, 0.9F));
    REQUIRE(events.size() == 2);
    CHECK(events[0].eventType == kTouchEventDown);
    CHECK(events[1].eventType == kTouchEventDown);
}

TEST_CASE("reset forgets the frame without emitting an UP", "[moonlight][touch]") {
    // reset() is for a pad that unbound or a session that ended: the host has
    // already forgotten the contacts, so synthesising an UP would address a
    // pointer on a session that is gone. The next real frame starts fresh.
    MoonlightTouchDiffer differ;
    REQUIRE(differ.diff(down(1, 0.5F, 0.5F), kUp).size() == 1);
    differ.reset();
    const auto events = differ.diff(down(1, 0.5F, 0.5F), kUp);
    REQUIRE(events.size() == 1);
    CHECK(events[0].eventType == kTouchEventDown);
}
