// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The engine rules (core/audio/AudioEnginePolicy.h): the mic and speaker
// eligibility truth tables, the exact-window reassembly of arbitrary capture
// chunks, and the drained-queue cushion refill. These decide when a
// microphone is OPEN, so the mic table is exhaustive: every one of the 32
// fact combinations is pinned, not sampled.

#include "core/audio/AudioEnginePolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numeric>
#include <vector>

using dish::audio::AudioSlotFacts;
using dish::audio::FrameWindower;
using dish::audio::micCaptureEligible;
using dish::audio::playoutRefillFrames;
using dish::audio::speakerPlayoutEligible;

TEST_CASE("mic capture needs every fact and no mute, exhaustively", "[audio][policy]") {
    for (int bits = 0; bits < 32; ++bits) {
        AudioSlotFacts f;
        f.streaming = (bits & 1) != 0;
        f.toggleOn = (bits & 2) != 0;
        f.routeMatched = (bits & 4) != 0;
        f.hostCarries = (bits & 8) != 0;
        f.muted = (bits & 16) != 0;
        const bool expected =
            f.streaming && f.toggleOn && f.routeMatched && f.hostCarries && !f.muted;
        INFO("bits " << bits);
        CHECK(micCaptureEligible(f) == expected);
    }
}

TEST_CASE("speaker playout ignores mute but needs everything else", "[audio][policy]") {
    // The asymmetry is deliberate: mute is about the user's voice leaving the
    // machine, not about the game reaching the pad.
    for (int bits = 0; bits < 32; ++bits) {
        AudioSlotFacts f;
        f.streaming = (bits & 1) != 0;
        f.toggleOn = (bits & 2) != 0;
        f.routeMatched = (bits & 4) != 0;
        f.hostCarries = (bits & 8) != 0;
        f.muted = (bits & 16) != 0;
        const bool expected = f.streaming && f.toggleOn && f.routeMatched && f.hostCarries;
        INFO("bits " << bits);
        CHECK(speakerPlayoutEligible(f) == expected);
    }
}

TEST_CASE("the playout constants are the contract's cushion", "[audio][policy]") {
    CHECK(dish::audio::kPlayoutStartThresholdFrames == 2);
    CHECK(dish::audio::kPlayoutBufferFrames == 4);
    // 960 frames x 2 channels x 2 bytes.
    CHECK(dish::audio::kPlayoutFrameBytes == 3840U);
}

TEST_CASE("the cushion refills only on a drained queue after start", "[audio][policy]") {
    // Before start the threshold owns the cushion; after it, only a genuinely
    // empty queue earns silence, and it earns exactly the start cushion.
    CHECK(playoutRefillFrames(/*started=*/false, /*queuedBytes=*/0) == 0);
    CHECK(playoutRefillFrames(true, 0) == dish::audio::kPlayoutStartThresholdFrames);
    CHECK(playoutRefillFrames(true, 1) == 0);
    CHECK(playoutRefillFrames(true, 4096) == 0);
}

// ── FrameWindower ────────────────────────────────────────────────────────────

namespace {

// Feed `chunks` and collect every emitted window's contents.
std::vector<std::vector<std::int16_t>>
collect(FrameWindower& w, const std::vector<std::vector<std::int16_t>>& chunks) {
    std::vector<std::vector<std::int16_t>> windows;
    for (const auto& chunk : chunks) {
        w.feed(chunk.data(), chunk.size(), [&](const std::int16_t* pcm, std::size_t frames) {
            windows.emplace_back(pcm, pcm + frames);
        });
    }
    return windows;
}

std::vector<std::int16_t> ramp(std::size_t count, std::int16_t start) {
    std::vector<std::int16_t> out(count);
    std::iota(out.begin(), out.end(), start);
    return out;
}

} // namespace

TEST_CASE("exact chunks pass straight through as whole windows", "[audio][windower]") {
    FrameWindower w(4);
    const auto windows = collect(w, {ramp(4, 0), ramp(4, 100)});
    REQUIRE(windows.size() == 2U);
    CHECK(windows[0] == ramp(4, 0));
    CHECK(windows[1] == ramp(4, 100));
    CHECK(w.pending() == 0U);
}

TEST_CASE("small chunks accumulate and never emit a partial window", "[audio][windower]") {
    // A partial window must never reach the encoder: the far end cannot place
    // one in its timeline.
    FrameWindower w(4);
    const auto windows = collect(w, {ramp(1, 0), ramp(2, 1), ramp(1, 3), ramp(3, 4)});
    REQUIRE(windows.size() == 1U);
    CHECK(windows[0] == ramp(4, 0));
    CHECK(w.pending() == 3U); // the tail waits for its window's last sample
}

TEST_CASE("an oversized chunk yields every whole window plus a carried tail", "[audio][windower]") {
    FrameWindower w(4);
    const auto windows = collect(w, {ramp(11, 0)});
    REQUIRE(windows.size() == 2U);
    CHECK(windows[0] == ramp(4, 0));
    CHECK(windows[1] == ramp(4, 4));
    CHECK(w.pending() == 3U);

    // The carried tail joins the next chunk in order, sample-exact.
    const auto more = collect(w, {ramp(1, 11)});
    REQUIRE(more.size() == 1U);
    CHECK(more[0] == ramp(4, 8));
}

TEST_CASE("a carried tail forces the copy path for following whole windows", "[audio][windower]") {
    // With a remainder in hand, a subsequent full-window-sized chunk must not
    // take the zero-copy fast path or samples would reorder.
    FrameWindower w(4);
    const auto windows = collect(w, {ramp(2, 0), ramp(4, 2), ramp(2, 6)});
    REQUIRE(windows.size() == 2U);
    CHECK(windows[0] == ramp(4, 0));
    CHECK(windows[1] == ramp(4, 4));
}

TEST_CASE("reset drops the carried tail", "[audio][windower]") {
    // A fresh stream must not inherit the old one's remainder.
    FrameWindower w(4);
    collect(w, {ramp(3, 0)});
    CHECK(w.pending() == 3U);
    w.reset();
    CHECK(w.pending() == 0U);
    const auto windows = collect(w, {ramp(4, 50)});
    REQUIRE(windows.size() == 1U);
    CHECK(windows[0] == ramp(4, 50));
}

TEST_CASE("null input and a missing sink are refused quietly", "[audio][windower]") {
    FrameWindower w(4);
    w.feed(nullptr, 8, [](const std::int16_t*, std::size_t) { FAIL("emitted from null input"); });
    const auto samples = ramp(8, 0);
    w.feed(samples.data(), samples.size(), {});
    CHECK(w.pending() == 0U);
}

TEST_CASE("the default window is the wire frame", "[audio][windower]") {
    FrameWindower w;
    std::size_t emitted = 0;
    const auto chunk = ramp(960, 0);
    w.feed(chunk.data(), chunk.size(),
           [&](const std::int16_t*, std::size_t frames) { emitted = frames; });
    CHECK(emitted == 960U);
}
