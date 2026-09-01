// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The host's two rumble streams both land on the pad's two motors, because no
// pad this client can Direct-claim has impulse-trigger motors. The rule that
// matters is the mixing one: last-writer-wins would let a trigger update cancel
// a body rumble that is still running, which is the "rumble flickers" bug on any
// host that drives both.

#include "core/moonlight/MoonlightTriggerRumble.h"

#include <catch2/catch_test_macros.hpp>

using dish::moonlight::BodyRumble;
using dish::moonlight::mixRumble;
using dish::moonlight::RumbleMix;
using dish::moonlight::withBodyRumble;
using dish::moonlight::withTriggerRumble;

TEST_CASE("a silent mix drives nothing", "[moonlight][triggerrumble]") {
    CHECK(mixRumble(RumbleMix{}) == BodyRumble{0, 0});
}

TEST_CASE("body rumble alone passes through unchanged", "[moonlight][triggerrumble]") {
    const auto mix = withBodyRumble(RumbleMix{}, 1000, 2000);
    CHECK(mixRumble(mix) == BodyRumble{1000, 2000});
}

TEST_CASE("left folds onto strong and right onto weak", "[moonlight][triggerrumble]") {
    // The mapping is the one dish-android's virtual pad uses, so a game feels
    // the same on all three clients.
    const auto mix = withTriggerRumble(RumbleMix{}, /*left=*/500, /*right=*/900);
    CHECK(mixRumble(mix) == BodyRumble{500, 900});
}

TEST_CASE("the louder of the two streams wins per motor", "[moonlight][triggerrumble]") {
    RumbleMix mix;
    mix = withBodyRumble(mix, 1000, 4000);
    mix = withTriggerRumble(mix, 3000, 2000);
    // strong: trigger 3000 beats body 1000. weak: body 4000 beats trigger 2000.
    CHECK(mixRumble(mix) == BodyRumble{3000, 4000});
}

TEST_CASE("a trigger update never cancels a live body rumble", "[moonlight][triggerrumble]") {
    // The regression this file exists for. A host that stops its trigger effect
    // while an explosion is still rumbling sends triggers 0,0; the body motors
    // must keep running.
    RumbleMix mix;
    mix = withBodyRumble(mix, 5000, 5000);
    mix = withTriggerRumble(mix, 8000, 8000);
    REQUIRE(mixRumble(mix) == BodyRumble{8000, 8000});
    mix = withTriggerRumble(mix, 0, 0);
    CHECK(mixRumble(mix) == BodyRumble{5000, 5000});
}

TEST_CASE("a body update never cancels a live trigger effect", "[moonlight][triggerrumble]") {
    RumbleMix mix;
    mix = withTriggerRumble(mix, 6000, 6000);
    mix = withBodyRumble(mix, 9000, 9000);
    REQUIRE(mixRumble(mix) == BodyRumble{9000, 9000});
    mix = withBodyRumble(mix, 0, 0);
    CHECK(mixRumble(mix) == BodyRumble{6000, 6000});
}

TEST_CASE("both streams stopping is a real stop", "[moonlight][triggerrumble]") {
    // Nothing may latch: a pad left buzzing after the game stopped is worse than
    // one that never buzzed.
    RumbleMix mix;
    mix = withBodyRumble(mix, 7000, 7000);
    mix = withTriggerRumble(mix, 7000, 7000);
    mix = withBodyRumble(mix, 0, 0);
    mix = withTriggerRumble(mix, 0, 0);
    CHECK(mixRumble(mix) == BodyRumble{0, 0});
}

TEST_CASE("the mix saturates at the wire maximum rather than wrapping",
          "[moonlight][triggerrumble]") {
    // Taking the max means the result can never exceed either input, so full
    // power on both streams is still full power and not an overflow to silence.
    RumbleMix mix;
    mix = withBodyRumble(mix, 0xFFFF, 0xFFFF);
    mix = withTriggerRumble(mix, 0xFFFF, 0xFFFF);
    CHECK(mixRumble(mix) == BodyRumble{0xFFFF, 0xFFFF});
}

TEST_CASE("the updaters touch only their own stream", "[moonlight][triggerrumble]") {
    RumbleMix mix;
    mix = withBodyRumble(mix, 11, 22);
    mix = withTriggerRumble(mix, 33, 44);
    CHECK(mix.bodyStrong == 11);
    CHECK(mix.bodyWeak == 22);
    CHECK(mix.triggerLeft == 33);
    CHECK(mix.triggerRight == 44);
    const auto after = withBodyRumble(mix, 55, 66);
    CHECK(after.triggerLeft == 33);
    CHECK(after.triggerRight == 44);
}
