// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The edge burst exists to heal a lost final frame after a state change.

#include "core/reducer/ResendPacer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using dish::reducer::ResendPacer;

namespace {

struct Harness {
    std::int64_t nowNs = 1'000'000'000;                 // non-zero so "never sent" != "sent at t=0"
    static constexpr std::int64_t kTickNs = 50'000'000; // the overlays' scheduler interval
    ResendPacer pacer{[this] { return nowNs; }};

    bool tick(bool changed) {
        nowNs += kTickNs;
        return pacer.resendDue(changed);
    }
};

} // namespace

TEST_CASE("ResendPacer: a change sends immediately plus the rest of the edge burst, then quiets",
          "[pacer]") {
    Harness h;
    REQUIRE(h.tick(/*changed=*/true));
    // kEdgeBurstResends sends total: the change tick + two unchanged ticks.
    REQUIRE(h.tick(false));
    REQUIRE(h.tick(false));
    REQUIRE_FALSE(h.tick(false));
    REQUIRE_FALSE(h.tick(false));
}

TEST_CASE("ResendPacer: steady state sends exactly one keepalive per interval", "[pacer]") {
    Harness h;
    REQUIRE(h.tick(true));
    for (int i = 0; i < ResendPacer::kEdgeBurstResends - 1; ++i) { REQUIRE(h.tick(false)); }

    int sends = 0;
    const int ticks = static_cast<int>(2 * ResendPacer::kKeepaliveIntervalNs / Harness::kTickNs);
    for (int i = 0; i < ticks; ++i) {
        if (h.tick(false)) { ++sends; }
    }
    REQUIRE(sends == 2);
}

TEST_CASE("ResendPacer: a change mid-burst restarts the burst from that tick", "[pacer]") {
    Harness h;
    REQUIRE(h.tick(true));
    REQUIRE(h.tick(false)); // burst tick 2 of 3
    REQUIRE(h.tick(true));
    REQUIRE(h.tick(false));
    REQUIRE(h.tick(false));
    REQUIRE_FALSE(h.tick(false));
}

TEST_CASE("ResendPacer: keepalive clock restarts from the last burst send", "[pacer]") {
    Harness h;
    REQUIRE(h.tick(true));
    for (int i = 0; i < ResendPacer::kEdgeBurstResends - 1; ++i) { REQUIRE(h.tick(false)); }
    const std::int64_t lastBurstSendNs = h.nowNs;

    // Just short of one keepalive interval after the LAST burst send.
    h.nowNs = lastBurstSendNs + ResendPacer::kKeepaliveIntervalNs - 2 * Harness::kTickNs;
    REQUIRE_FALSE(h.tick(false));
    REQUIRE(h.tick(false));
}
