// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/audio/BenchAudio.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kStereo = 2;
constexpr int kPadEndpoint = 4;

int loudestOf(const std::vector<std::int16_t>& samples, std::size_t from, std::size_t to,
              std::size_t channel, std::size_t stride) {
    int loudest = 0;
    for (std::size_t frame = from; frame < to; ++frame) {
        loudest = std::max(loudest, std::abs(static_cast<int>(samples[frame * stride + channel])));
    }
    return loudest;
}

// Rising zero crossings over a span of frames, which at 48 kHz count the tone's cycles.
int risingCrossings(std::size_t from, std::size_t to) {
    int crossings = 0;
    for (std::size_t frame = from + 1; frame < to; ++frame) {
        const auto before = dish::audio::benchToneSampleAt(static_cast<int>(frame - 1));
        const auto now = dish::audio::benchToneSampleAt(static_cast<int>(frame));
        if (before <= 0 && now > 0) { ++crossings; }
    }
    return crossings;
}

} // namespace

TEST_CASE("the test tone lasts 800 ms at 48 kHz", "[diagnostics][bench][audio]") {
    const auto tone = dish::audio::benchTone(kStereo, dish::audio::kSpeakerLaneOffset);
    CHECK(tone.size() == static_cast<std::size_t>(38400 * kStereo));
}

TEST_CASE("the test tone is 660 Hz for its first half and 880 Hz for its second",
          "[diagnostics][bench][audio]") {
    // 400 ms of each note: 264 cycles of 660 Hz, then 352 of 880 Hz, give or take the edges.
    const int first = risingCrossings(0, 19200);
    const int second = risingCrossings(19200, 38400);
    CHECK(std::abs(first - 264) <= 1);
    CHECK(std::abs(second - 352) <= 1);
}

TEST_CASE("the test tone peaks at 0.35 of full scale and fades in and out over 10 ms",
          "[diagnostics][bench][audio]") {
    const auto tone = dish::audio::benchTone(kStereo, dish::audio::kSpeakerLaneOffset);
    const int fullScaleTone = static_cast<int>(0.35 * 32767);
    CHECK(loudestOf(tone, 0, 38400, 0, kStereo) <= fullScaleTone);
    CHECK(loudestOf(tone, 480, 18720, 0, kStereo) >= fullScaleTone - 2);
    CHECK(dish::audio::benchToneSampleAt(0) == 0);
    CHECK(loudestOf(tone, 0, 48, 0, kStereo) < fullScaleTone / 8);
    CHECK(loudestOf(tone, 38400 - 48, 38400, 0, kStereo) < fullScaleTone / 8);
}

TEST_CASE("the speaker's tone plays on the endpoint's first pair and leaves the actuators silent",
          "[diagnostics][bench][audio]") {
    const auto tone = dish::audio::benchTone(kPadEndpoint, dish::audio::kSpeakerLaneOffset);
    REQUIRE(tone.size() == static_cast<std::size_t>(38400 * kPadEndpoint));
    CHECK(loudestOf(tone, 0, 38400, 0, kPadEndpoint) > 0);
    CHECK(loudestOf(tone, 0, 38400, 1, kPadEndpoint) > 0);
    CHECK(loudestOf(tone, 0, 38400, 2, kPadEndpoint) == 0);
    CHECK(loudestOf(tone, 0, 38400, 3, kPadEndpoint) == 0);
}

TEST_CASE("the haptics tone plays on the actuator pair and leaves the speaker silent",
          "[diagnostics][bench][audio]") {
    const auto tone = dish::audio::benchTone(kPadEndpoint, dish::audio::kHapticLaneOffset);
    CHECK(loudestOf(tone, 0, 38400, 0, kPadEndpoint) == 0);
    CHECK(loudestOf(tone, 0, 38400, 1, kPadEndpoint) == 0);
    CHECK(loudestOf(tone, 0, 38400, 2, kPadEndpoint) > 0);
    CHECK(loudestOf(tone, 0, 38400, 3, kPadEndpoint) > 0);
}

TEST_CASE("an endpoint without the lane's pair gets no tone at all",
          "[diagnostics][bench][audio]") {
    CHECK(dish::audio::benchTone(kStereo, dish::audio::kHapticLaneOffset).empty());
    CHECK(dish::audio::benchTone(1, dish::audio::kSpeakerLaneOffset).empty());
}

TEST_CASE("a silent window reads as an empty meter", "[diagnostics][bench][audio]") {
    const std::vector<std::int16_t> silence(960, 0);
    const auto reading = dish::audio::micReadingOf(silence.data(), silence.size());
    CHECK(reading.rms == 0.0F);
    CHECK(reading.peak == 0.0F);
    CHECK(dish::audio::micMeterOf(reading.rms) == 0.0F);
    CHECK(dish::audio::micReadingOf(nullptr, 0) == dish::audio::MicReading{});
}

TEST_CASE("a full-scale window fills the meter and the peak", "[diagnostics][bench][audio]") {
    std::vector<std::int16_t> square(960);
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = (i % 2 == 0) ? std::int16_t{-32768} : std::int16_t{32767};
    }
    const auto reading = dish::audio::micReadingOf(square.data(), square.size());
    CHECK(reading.peak == 1.0F);
    CHECK(dish::audio::micPercentOf(dish::audio::micMeterOf(reading.rms)) == 100);
}

TEST_CASE("the meter reads -30 dBFS as half full", "[diagnostics][bench][audio]") {
    const float minus30 = std::pow(10.0F, -30.0F / 20.0F);
    CHECK(dish::audio::micPercentOf(dish::audio::micMeterOf(minus30)) == 50);
    CHECK(dish::audio::micMeterOf(std::pow(10.0F, -70.0F / 20.0F)) == 0.0F);
}

TEST_CASE("the meter falls back a step per window and the peak keeps the loudest window",
          "[diagnostics][bench][audio]") {
    const dish::audio::MicMeter loud{0.8F, 0.9F};
    const dish::audio::MicMeter next = dish::audio::nextMicMeter(loud, dish::audio::MicReading{});
    CHECK(std::abs(next.meter - 0.76F) < 1e-6F);
    CHECK(next.peak == 0.9F);
    const dish::audio::MicMeter louder =
        dish::audio::nextMicMeter(next, dish::audio::MicReading{1.0F, 1.0F});
    CHECK(louder.meter == 1.0F);
    CHECK(louder.peak == 1.0F);
}

TEST_CASE("a level shows as a whole percent between 0 and 100", "[diagnostics][bench][audio]") {
    CHECK(dish::audio::micPercentOf(0.004F) == 0);
    CHECK(dish::audio::micPercentOf(0.505F) == 51);
    CHECK(dish::audio::micPercentOf(1.2F) == 100);
    CHECK(dish::audio::micPercentOf(-0.5F) == 0);
}
