// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// "Is anyone actually playing?" — the bool the timed keep-awake mode hangs on.
// Both the counter and the clock are injected, so every case here is exact
// rather than timing-dependent: no sleeps, no wall clock, and the idle-window
// boundary is asserted on the millisecond rather than approached.

#include "source/input/ControllerActivitySource.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using dish::reducer::kKeepAwakeMaxTimeoutMinutes;
using dish::reducer::kKeepAwakeMinTimeoutMinutes;
using dish::source::ControllerActivitySource;
using dish::test::StateSourceProbe;

namespace {

constexpr std::int64_t kMinute = 60'000;
// The default idle window, in milliseconds.
constexpr std::int64_t kDefaultWindow = 5 * kMinute;

} // namespace

TEST_CASE("ControllerActivitySource: starts inactive", "[input][keepawake]") {
    std::uint64_t counter = 0;
    const ControllerActivitySource src([&] { return counter; });
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: a sample with no input keeps it inactive",
          "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    src.sampleAt(1000);
    src.sampleAt(kDefaultWindow * 4);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: a counter bump at sample time is activity",
          "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    counter = 1;
    src.sampleAt(1000);
    REQUIRE(src.state().value());
}

TEST_CASE("ControllerActivitySource: a counter already turning at construction is not activity",
          "[input][keepawake]") {
    // The ctor seeds its baseline from the counter, so a pad that was played
    // with before this source existed does not read as a live session.
    std::uint64_t counter = 4096;
    ControllerActivitySource src([&] { return counter; });
    src.sampleAt(1000);
    REQUIRE_FALSE(src.state().value());

    counter = 4097;
    src.sampleAt(2000);
    REQUIRE(src.state().value());
}

TEST_CASE("ControllerActivitySource: sitting still past the window goes inactive",
          "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    counter = 1;
    src.sampleAt(1000);
    REQUIRE(src.state().value());

    src.sampleAt(1000 + kDefaultWindow);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: the window boundary is exclusive", "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    counter = 1;
    src.sampleAt(1000);

    src.sampleAt(1000 + kDefaultWindow - 1);
    REQUIRE(src.state().value());
    src.sampleAt(1000 + kDefaultWindow);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: input inside the window re-arms it", "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    counter = 1;
    src.sampleAt(kMinute);

    counter = 2;
    src.sampleAt(5 * kMinute); // still inside the window, and it slides forward
    REQUIRE(src.state().value());
    src.sampleAt(9 * kMinute);
    REQUIRE(src.state().value()); // 4 minutes since the last input, not 9
    src.sampleAt(10 * kMinute + 1);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: noteActivityAt is activity on its own", "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    // A session opening counts: the pad the user is picking up has not reported
    // yet, so the counter is still where the ctor left it.
    src.noteActivityAt(5000);
    REQUIRE(src.state().value());

    src.sampleAt(5000 + kDefaultWindow);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: setIdleTimeoutMinutes moves the window",
          "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    src.setIdleTimeoutMinutes(1);
    src.noteActivityAt(kMinute);
    REQUIRE(src.state().value());

    src.sampleAt(2 * kMinute - 1);
    REQUIRE(src.state().value());
    src.sampleAt(2 * kMinute);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: setIdleTimeoutMinutes clamps both ends",
          "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });

    // 0 would mean "never expires" to the reducer; the clamp is what stops a
    // bad stored value pinning the machine awake forever.
    src.setIdleTimeoutMinutes(0);
    src.noteActivityAt(kMinute);
    src.sampleAt(kMinute + kKeepAwakeMinTimeoutMinutes * kMinute - 1);
    REQUIRE(src.state().value());
    src.sampleAt(kMinute + kKeepAwakeMinTimeoutMinutes * kMinute);
    REQUIRE_FALSE(src.state().value());

    src.setIdleTimeoutMinutes(100000);
    src.noteActivityAt(kMinute);
    src.sampleAt(kMinute + kKeepAwakeMaxTimeoutMinutes * kMinute - 1);
    REQUIRE(src.state().value());
    src.sampleAt(kMinute + kKeepAwakeMaxTimeoutMinutes * kMinute);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: a still pad sampled repeatedly emits once",
          "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    StateSourceProbe<bool> probe(src.state());
    // The one emission is the current value replayed to the new subscriber.
    REQUIRE(probe.count() == 1);

    for (int i = 1; i <= 10; ++i) { src.sampleAt(i * 1000); }
    // Distinct-until-changed, or the composer downstream would recompute — and
    // the inhibitor would be poked — on every 1 Hz tick.
    REQUIRE(probe.count() == 1);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: only the edges reach subscribers", "[input][keepawake]") {
    std::uint64_t counter = 0;
    ControllerActivitySource src([&] { return counter; });
    StateSourceProbe<bool> probe(src.state());

    counter = 1;
    src.sampleAt(1000);
    REQUIRE(probe.count() == 2);
    REQUIRE(src.state().value());

    // Still playing: several bumps, all inside the window, one steady true.
    for (int i = 1; i <= 5; ++i) {
        counter = static_cast<std::uint64_t>(1 + i);
        src.sampleAt(1000 + i * 1000);
    }
    REQUIRE(probe.count() == 2);

    src.sampleAt(6000 + kDefaultWindow);
    REQUIRE(probe.count() == 3);
    REQUIRE_FALSE(src.state().value());
}

TEST_CASE("ControllerActivitySource: tolerates an empty counter", "[input][keepawake]") {
    // AppModel can build this before the input processor exists.
    ControllerActivitySource src(ControllerActivitySource::ActuationCounter{});
    src.sampleAt(1000);
    REQUIRE_FALSE(src.state().value());
    src.noteActivityAt(2000);
    REQUIRE(src.state().value());
}
