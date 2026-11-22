// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Rules 2 and 3 of the bench (docs/diagnostics-bundle.md, "The bench"): the state a test borrows
// goes back to what its owner last sent, and a value sent while the test holds the surface is
// remembered, not written.

#include "core/reducer/BenchSurfaces.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using dish::reducer::BenchSurface;
using dish::reducer::LightbarValue;
using dish::reducer::RememberedSurfaces;
using dish::reducer::SurfaceLedger;
using dish::reducer::SurfaceWrite;
using dish::reducer::TriggerEffectsValue;

namespace {

// What a test's end wrote, in order.
struct Writes {
    std::vector<SurfaceWrite> seen;
    void operator()(const SurfaceWrite& write) { seen.push_back(write); }
};

TriggerEffectsValue someEffect() {
    TriggerEffectsValue value;
    value.left[0] = 0x26;
    value.right[0] = 0x21;
    value.right[3] = 0x7F;
    return value;
}

} // namespace

TEST_CASE("a surface the owner set goes back to the owner's last value", "[diagnostics][bench]") {
    const RememberedSurfaces remembered =
        dish::reducer::withLightbar(dish::reducer::withLightbar({}, {1, 2, 3}), {4, 5, 6});
    const SurfaceWrite write = dish::reducer::restoreWriteFor(BenchSurface::Lightbar, remembered);
    CHECK(write.surface == BenchSurface::Lightbar);
    CHECK(write.values.lightbar == LightbarValue{4, 5, 6});
}

TEST_CASE("a surface nothing ever set goes back to the idle state", "[diagnostics][bench]") {
    const RememberedSurfaces lightbarOnly = dish::reducer::withLightbar({}, {9, 9, 9});
    CHECK(
        dish::reducer::restoreWriteFor(BenchSurface::PlayerLeds, lightbarOnly).values.playerLeds ==
        0x00);
    CHECK(dish::reducer::restoreWriteFor(BenchSurface::MicLamp, lightbarOnly).values.micLamp ==
          dish::proto::kMicLedStateOff);
    CHECK(dish::reducer::restoreWriteFor(BenchSurface::TriggerEffects, lightbarOnly)
              .values.triggerEffects == TriggerEffectsValue{});
    CHECK(dish::reducer::restoreWriteFor(BenchSurface::Lightbar, {}).values.lightbar ==
          LightbarValue{0, 0, 0});
}

TEST_CASE("every surface remembers its own last value", "[diagnostics][bench]") {
    RememberedSurfaces remembered;
    remembered = dish::reducer::withPlayerLeds(remembered, 0x04);
    remembered = dish::reducer::withTriggerEffects(remembered, someEffect());
    remembered = dish::reducer::withMicLamp(remembered, dish::proto::kMicLedStatePulse);
    CHECK(dish::reducer::ownerSet(remembered, BenchSurface::PlayerLeds));
    CHECK(dish::reducer::ownerSet(remembered, BenchSurface::TriggerEffects));
    CHECK(dish::reducer::ownerSet(remembered, BenchSurface::MicLamp));
    CHECK_FALSE(dish::reducer::ownerSet(remembered, BenchSurface::Lightbar));
    CHECK(remembered.values.playerLeds == 0x04);
    CHECK(remembered.values.triggerEffects == someEffect());
    CHECK(remembered.values.micLamp == dish::proto::kMicLedStatePulse);
}

TEST_CASE("a value for a surface no test holds is written at once", "[diagnostics][bench]") {
    SurfaceLedger ledger;
    CHECK(ledger.rememberLightbar({1, 2, 3}));
    CHECK(ledger.rememberPlayerLeds(0x01));
    CHECK(ledger.rememberTriggerEffects(someEffect()));
    CHECK(ledger.rememberMicLamp(dish::proto::kMicLedStateOn));
    const RememberedSurfaces seen = ledger.snapshot().surfaces;
    CHECK(seen.values.lightbar == LightbarValue{1, 2, 3});
    CHECK(seen.values.playerLeds == 0x01);
    CHECK(seen.values.triggerEffects == someEffect());
    CHECK(seen.values.micLamp == dish::proto::kMicLedStateOn);
}

TEST_CASE("a value for a held surface is remembered, not written", "[diagnostics][bench]") {
    SurfaceLedger ledger;
    ledger.hold(BenchSurface::Lightbar);
    CHECK(ledger.holds(BenchSurface::Lightbar));
    CHECK_FALSE(ledger.rememberLightbar({7, 8, 9}));
    CHECK(ledger.snapshot().surfaces.values.lightbar == LightbarValue{7, 8, 9});
}

TEST_CASE("while a test holds one surface, the others keep following the game",
          "[diagnostics][bench]") {
    SurfaceLedger ledger;
    ledger.hold(BenchSurface::TriggerEffects);
    CHECK(ledger.rememberLightbar({1, 1, 1}));
    CHECK(ledger.rememberPlayerLeds(0x02));
    CHECK(ledger.rememberMicLamp(dish::proto::kMicLedStateOn));
    CHECK_FALSE(ledger.rememberTriggerEffects(someEffect()));
}

TEST_CASE("a test's end writes back the value the game sent while it held the surface",
          "[diagnostics][bench]") {
    SurfaceLedger ledger;
    CHECK(ledger.rememberPlayerLeds(0x01));
    ledger.hold(BenchSurface::PlayerLeds);
    CHECK_FALSE(ledger.rememberPlayerLeds(0x08));
    Writes writes;
    dish::reducer::giveBack(ledger, BenchSurface::PlayerLeds, writes);
    REQUIRE(writes.seen.size() == 1U);
    CHECK(writes.seen.front().surface == BenchSurface::PlayerLeds);
    CHECK(writes.seen.front().values.playerLeds == 0x08);
    CHECK_FALSE(ledger.holds(BenchSurface::PlayerLeds));
    CHECK(ledger.rememberPlayerLeds(0x10));
}

TEST_CASE("a test's end on a surface the game never set writes the idle state",
          "[diagnostics][bench]") {
    SurfaceLedger ledger;
    ledger.hold(BenchSurface::MicLamp);
    Writes writes;
    dish::reducer::giveBack(ledger, BenchSurface::MicLamp, writes);
    REQUIRE(writes.seen.size() == 1U);
    CHECK(writes.seen.front().values.micLamp == dish::proto::kMicLedStateOff);
}

TEST_CASE("a value remembered while the end writes is written again", "[diagnostics][bench]") {
    SurfaceLedger ledger;
    ledger.hold(BenchSurface::Lightbar);
    CHECK_FALSE(ledger.rememberLightbar({1, 0, 0}));
    std::vector<LightbarValue> written;
    // The game's next colour lands while the first write back is on its way.
    bool raced = false;
    dish::reducer::giveBack(ledger, BenchSurface::Lightbar, [&](const SurfaceWrite& write) {
        written.push_back(write.values.lightbar);
        if (!raced) {
            raced = true;
            CHECK(ledger.rememberLightbar({0, 1, 0}));
        }
    });
    REQUIRE(written.size() == 2U);
    CHECK(written[0] == LightbarValue{1, 0, 0});
    CHECK(written[1] == LightbarValue{0, 1, 0});
}

namespace {

// A colour per count, so a later value is a larger number.
LightbarValue colourFor(std::uint32_t count) {
    return LightbarValue{static_cast<std::uint8_t>((count >> 16) & 0xFFU),
                         static_cast<std::uint8_t>((count >> 8) & 0xFFU),
                         static_cast<std::uint8_t>(count & 0xFFU)};
}

std::uint32_t countOf(const LightbarValue& colour) {
    return (static_cast<std::uint32_t>(colour.r) << 16) |
           (static_cast<std::uint32_t>(colour.g) << 8) | colour.b;
}

// A game's colour stream, a message every few tens of microseconds, written to the pad whenever
// the ledger says so.
class PacedSender {
  public:
    PacedSender(SurfaceLedger& ledger, std::atomic<std::uint32_t>& pad)
        : ledger_(ledger), pad_(pad), thread_(&PacedSender::run, this) {}
    ~PacedSender() { stopAndJoin(); }

    PacedSender(const PacedSender&) = delete;
    PacedSender& operator=(const PacedSender&) = delete;
    PacedSender(PacedSender&&) = delete;
    PacedSender& operator=(PacedSender&&) = delete;

    void stopAndJoin() {
        stop_.store(true);
        if (thread_.joinable()) { thread_.join(); }
    }
    std::uint32_t lastSent() const { return lastSent_.load(); }

  private:
    void run() {
        std::uint32_t count = 0;
        while (!stop_.load()) {
            ++count;
            if (ledger_.rememberLightbar(colourFor(count))) { pad_.store(count); }
            lastSent_.store(count);
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }

    SurfaceLedger& ledger_;
    std::atomic<std::uint32_t>& pad_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint32_t> lastSent_{0};
    std::thread thread_;
};

} // namespace

TEST_CASE("no colour the game sent while a test held the bar is lost when the test ends",
          "[diagnostics][bench]") {
    constexpr int kRounds = 200;
    SurfaceLedger ledger;
    std::atomic<std::uint32_t> pad{0};
    PacedSender sender(ledger, pad);
    for (int round = 0; round < kRounds; ++round) {
        ledger.hold(BenchSurface::Lightbar);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        dish::reducer::giveBack(ledger, BenchSurface::Lightbar, [&](const SurfaceWrite& write) {
            pad.store(countOf(write.values.lightbar));
        });
    }
    sender.stopAndJoin();
    CHECK(pad.load() == sender.lastSent());
    CHECK(countOf(ledger.snapshot().surfaces.values.lightbar) == sender.lastSent());
}
