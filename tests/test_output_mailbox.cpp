// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/input/OutputMailbox.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

using dish::input::OutputMailbox;
namespace output = dish::input::output;

TEST_CASE("an untouched mailbox has nothing to take", "[output-mailbox]") {
    OutputMailbox box;
    CHECK_FALSE(box.pending());
    CHECK_FALSE(box.take().has_value());
}

TEST_CASE("the newest rumble wins over the ones before it", "[output-mailbox]") {
    OutputMailbox box;
    box.postRumble(100, 200, 10);
    box.postRumble(300, 400, 20);
    box.postRumble(500, 600, 30);
    const auto taken = box.take();
    REQUIRE(taken.has_value());
    CHECK(taken->kinds == output::kRumble);
    CHECK(taken->state.strong == 500);
    CHECK(taken->state.weak == 600);
    CHECK(taken->state.durationMs == 30);
}

TEST_CASE("each kind posted sets its own pending bit, and a take clears them all",
          "[output-mailbox]") {
    OutputMailbox box;
    box.postLightbar(1, 2, 3);
    box.postPlayerLeds(0x15);
    box.postMicLed(2);
    std::array<std::uint8_t, dish::input::usbout::kTriggerEffectBlockBytes> left{};
    left.fill(7);
    std::array<std::uint8_t, dish::input::usbout::kTriggerEffectBlockBytes> right{};
    right.fill(9);
    box.postTriggerEffects(left, right);
    CHECK(box.pending());
    const auto taken = box.take();
    REQUIRE(taken.has_value());
    CHECK(taken->kinds ==
          (output::kLightbar | output::kPlayerLeds | output::kMicLed | output::kTriggerEffects));
    CHECK(taken->state.b == 3);
    CHECK(taken->state.ledMask == 0x15);
    CHECK(taken->state.micLedState == 2);
    CHECK(taken->state.leftTrigger == left);
    CHECK(taken->state.rightTrigger == right);
    CHECK_FALSE(box.pending());
    CHECK_FALSE(box.take().has_value());
}

TEST_CASE("a kind not posted again keeps its value for the next take", "[output-mailbox]") {
    OutputMailbox box;
    box.postLightbar(10, 20, 30);
    REQUIRE(box.take().has_value());
    box.postRumble(1, 1, 1);
    const auto taken = box.take();
    REQUIRE(taken.has_value());
    CHECK(taken->kinds == output::kRumble);
    CHECK(taken->state.r == 10);
}

TEST_CASE("producers on two threads never leave the owner a torn state", "[output-mailbox]") {
    OutputMailbox box;
    std::atomic<bool> stop{false};
    const auto produce = [&](std::uint16_t base) {
        for (std::uint16_t n = base; !stop.load(std::memory_order_relaxed);
             n = static_cast<std::uint16_t>(n + 2)) {
            box.postRumble(n, n, n);
        }
    };
    std::thread even(produce, std::uint16_t{0});
    std::thread odd(produce, std::uint16_t{1});
    int taken = 0;
    bool whole = true;
    while (taken < 20'000) {
        const auto t = box.take();
        if (!t.has_value()) { continue; }
        whole = whole && t->state.strong == t->state.weak && t->state.weak == t->state.durationMs;
        ++taken;
    }
    stop.store(true);
    even.join();
    odd.join();
    CHECK(whole);
}

TEST_CASE("a cleared mailbox has nothing to take until the next post", "[output-mailbox]") {
    OutputMailbox box;
    box.postRumble(1, 2, 3);
    box.clear();
    CHECK_FALSE(box.take().has_value());
    box.postLightbar(4, 5, 6);
    const auto taken = box.take();
    REQUIRE(taken.has_value());
    CHECK(taken->kinds == output::kLightbar);
}
