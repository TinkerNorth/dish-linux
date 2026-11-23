// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The bench's audio (docs/diagnostics-bundle.md, "The bench"): the test tone,
// 660 Hz then 880 Hz, 800 ms in all at amplitude 0.35 with 10 ms fades, on
// one lane pair of the pad's own endpoint (the speaker's, or the haptic
// actuators' for `haptics.play`), and the microphone's level as a meter and a
// peak. Both are dish-android's inspector's, sample for sample and step for
// step, so the same pad sounds and reads the same on either client.

#pragma once

#include "core/model/Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <vector>

namespace dish::audio {

inline constexpr double kToneFirstHz = 660.0;
inline constexpr double kToneSecondHz = 880.0;
inline constexpr double kToneAmplitude = 0.35;
inline constexpr int kToneMs = 800;
inline constexpr int kToneFadeMs = 10;
inline constexpr int kMsPerSecond = 1000;
// One sample per channel each; the two notes are of equal length.
inline constexpr int kToneFrames = proto::kAudioSampleRateHz / kMsPerSecond * kToneMs;
inline constexpr int kToneNoteFrames = kToneFrames / 2;
inline constexpr int kToneFadeFrames = proto::kAudioSampleRateHz / kMsPerSecond * kToneFadeMs;

// Where each lane pair sits on the pad's endpoint: the speaker's first, the actuators' after it.
inline constexpr int kSpeakerLaneOffset = 0;
inline constexpr int kHapticLaneOffset = proto::kAudioSpeakerChannels;

// The tone's sample at one frame, truncated toward zero as dish-android's is.
inline std::int16_t benchToneSampleAt(int frame) {
    const int inNote = frame % kToneNoteFrames;
    const double hz = frame < kToneNoteFrames ? kToneFirstHz : kToneSecondHz;
    const int fromEdge = std::min(inNote, kToneNoteFrames - 1 - inNote);
    const double envelope =
        std::min(1.0, static_cast<double>(fromEdge) / static_cast<double>(kToneFadeFrames));
    const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(inNote) /
                         static_cast<double>(proto::kAudioSampleRateHz);
    const double fullScale = std::numeric_limits<std::int16_t>::max();
    return static_cast<std::int16_t>(std::sin(phase) * kToneAmplitude * envelope * fullScale);
}

// The whole tone, interleaved at `channels`, on the lane pair at `laneOffset` with every other
// channel silent. Empty for an endpoint without that pair.
inline std::vector<std::int16_t> benchTone(int channels, int laneOffset) {
    const bool pairFits = laneOffset >= 0 && laneOffset + proto::kAudioSpeakerChannels <= channels;
    if (!pairFits) { return {}; }
    const auto stride = static_cast<std::size_t>(channels);
    const auto offset = static_cast<std::size_t>(laneOffset);
    std::vector<std::int16_t> out(static_cast<std::size_t>(kToneFrames) * stride, 0);
    for (int frame = 0; frame < kToneFrames; ++frame) {
        const std::int16_t sample = benchToneSampleAt(frame);
        const std::size_t at = static_cast<std::size_t>(frame) * stride + offset;
        out[at] = sample;
        out[at + 1] = sample;
    }
    return out;
}

// One 20 ms window of the microphone, as fractions of full scale.
struct MicReading {
    float rms = 0.0F;
    float peak = 0.0F;

    bool operator==(const MicReading&) const = default;
};

inline MicReading micReadingOf(const std::int16_t* samples, std::size_t count) {
    if (samples == nullptr || count == 0) { return {}; }
    int peak = 0;
    double sumSquares = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const int sample = samples[i];
        peak = std::max(peak, std::abs(sample));
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const double fullScale = -static_cast<double>(std::numeric_limits<std::int16_t>::min());
    const double rms = std::sqrt(sumSquares / static_cast<double>(count)) / fullScale;
    return MicReading{static_cast<float>(rms), static_cast<float>(peak / fullScale)};
}

// The meter reads the window's level on a 60 dB scale: -60 dBFS and below is empty, 0 dBFS full.
inline constexpr float kMicFloorDb = -60.0F;
inline constexpr float kDecibelsPerDecade = 20.0F;
inline constexpr float kMicMeterDecay = 0.04F;

inline float micMeterOf(float rms) {
    if (rms <= 0.0F) { return 0.0F; }
    const float db = kDecibelsPerDecade * std::log10(rms);
    return std::clamp((db - kMicFloorDb) / -kMicFloorDb, 0.0F, 1.0F);
}

// What the page shows: the meter falls back by a step per window rather than at once, and the
// peak keeps the loudest window of the run.
struct MicMeter {
    float meter = 0.0F;
    float peak = 0.0F;

    bool operator==(const MicMeter&) const = default;
};

inline MicMeter nextMicMeter(const MicMeter& shown, const MicReading& reading) {
    return MicMeter{std::max(micMeterOf(reading.rms), shown.meter - kMicMeterDecay),
                    std::max(shown.peak, reading.peak)};
}

inline constexpr float kPercent = 100.0F;

inline int micPercentOf(float fraction) {
    const long rounded = std::lround(fraction * kPercent);
    return static_cast<int>(std::clamp(rounded, 0L, static_cast<long>(kPercent)));
}

} // namespace dish::audio
