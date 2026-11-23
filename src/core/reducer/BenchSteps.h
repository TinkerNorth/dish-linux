// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What each bench test writes, and when (docs/diagnostics-bundle.md, "The
// bench"): the contract's strengths, colours, masks and times, which are
// dish-android's inspector's, and the shape each test's run takes.

#pragma once

#include "core/reducer/BenchItems.h"
#include "core/reducer/BenchSurfaces.h"
#include "core/reducer/FeedbackBench.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace dish::reducer {

// Each step of a cycle shows for this long.
inline constexpr std::int64_t kBenchStepMs = 400;
inline constexpr std::array<LightbarValue, 3> kLightbarCycle{{
    {0xFF, 0x00, 0x00},
    {0x00, 0xFF, 0x00},
    {0x00, 0x00, 0xFF},
}};
inline constexpr std::array<std::uint8_t, 5> kPlayerLedCycle{0x01, 0x02, 0x04, 0x08, 0x10};

// The adaptive triggers and the mic lamp hold their one step this long.
inline constexpr std::int64_t kStateHoldMs = 2000;

// A DualSense effect block: the mode at byte 0, the force at byte 2. Mode 01 is rigid resistance.
inline constexpr std::uint8_t kTriggerModeRigid = 0x01;
inline constexpr std::size_t kTriggerModeByte = 0;
inline constexpr std::size_t kTriggerForceByte = 2;
inline constexpr std::uint8_t kTriggerForceFull = 0xFF;

// The microphone's level shows for at most this long.
inline constexpr std::int64_t kMicLevelMs = 8000;

inline TriggerEffectsValue rigidTriggerEffects() {
    TriggerBlock block{};
    block[kTriggerModeByte] = kTriggerModeRigid;
    block[kTriggerForceByte] = kTriggerForceFull;
    return TriggerEffectsValue{block, block};
}

// The shape of a test's run, which decides who drives it: the router's motors, a surface the run
// borrows, the pad's own audio endpoint, or the inspector's stick bench.
enum class BenchRunKind : std::uint8_t { Buzz, TriggerBuzz, Surface, Tone, MicLevel, Stick };

inline BenchRunKind benchRunKindOf(BenchTest test) {
    switch (benchItemOf(test)) {
    case BenchItem::Rumble:
        return BenchRunKind::Buzz;
    case BenchItem::TriggerRumble:
        return BenchRunKind::TriggerBuzz;
    case BenchItem::Lightbar:
    case BenchItem::PlayerLeds:
    case BenchItem::AdaptiveTriggers:
    case BenchItem::MicLamp:
        return BenchRunKind::Surface;
    case BenchItem::Speaker:
    case BenchItem::Haptics:
        return BenchRunKind::Tone;
    case BenchItem::Microphone:
        return BenchRunKind::MicLevel;
    case BenchItem::Sticks:
        return BenchRunKind::Stick;
    }
    return BenchRunKind::Stick;
}

// Which motors a buzz test runs; none for any other test.
inline std::optional<BuzzMotor> buzzMotorOf(BenchTest test) {
    switch (test) {
    case BenchTest::RumbleWeak:
        return BuzzMotor::Weak;
    case BenchTest::RumbleStrong:
        return BuzzMotor::Strong;
    case BenchTest::RumbleBoth:
        return BuzzMotor::Both;
    case BenchTest::TriggerRumbleLeft:
    case BenchTest::TriggerRumbleRight:
    case BenchTest::TriggerRumbleBoth:
    case BenchTest::LightbarCycle:
    case BenchTest::PlayerLedsCycle:
    case BenchTest::TriggerPulse:
    case BenchTest::LampOn:
    case BenchTest::LampPulse:
    case BenchTest::TonePlay:
    case BenchTest::HapticsPlay:
    case BenchTest::MicLevel:
    case BenchTest::StickDrift:
    case BenchTest::StickRange:
        return std::nullopt;
    }
    return std::nullopt;
}

inline std::optional<TriggerMotor> triggerMotorOf(BenchTest test) {
    switch (test) {
    case BenchTest::TriggerRumbleLeft:
        return TriggerMotor::Left;
    case BenchTest::TriggerRumbleRight:
        return TriggerMotor::Right;
    case BenchTest::TriggerRumbleBoth:
        return TriggerMotor::Both;
    case BenchTest::RumbleWeak:
    case BenchTest::RumbleStrong:
    case BenchTest::RumbleBoth:
    case BenchTest::LightbarCycle:
    case BenchTest::PlayerLedsCycle:
    case BenchTest::TriggerPulse:
    case BenchTest::LampOn:
    case BenchTest::LampPulse:
    case BenchTest::TonePlay:
    case BenchTest::HapticsPlay:
    case BenchTest::MicLevel:
    case BenchTest::StickDrift:
    case BenchTest::StickRange:
        return std::nullopt;
    }
    return std::nullopt;
}

// A surface test's run: the surface it holds, how many steps it writes (step i at
// i * kBenchStepMs) and when it gives the surface back.
struct SurfacePlan {
    BenchSurface surface = BenchSurface::Lightbar;
    std::size_t steps = 0;
    std::int64_t holdMs = 0;

    bool operator==(const SurfacePlan&) const = default;
};

inline constexpr std::int64_t kLightbarCycleMs =
    static_cast<std::int64_t>(kLightbarCycle.size()) * kBenchStepMs;
inline constexpr std::int64_t kPlayerLedCycleMs =
    static_cast<std::int64_t>(kPlayerLedCycle.size()) * kBenchStepMs;

inline std::optional<SurfacePlan> surfacePlanFor(BenchTest test) {
    switch (test) {
    case BenchTest::LightbarCycle:
        return SurfacePlan{BenchSurface::Lightbar, kLightbarCycle.size(), kLightbarCycleMs};
    case BenchTest::PlayerLedsCycle:
        return SurfacePlan{BenchSurface::PlayerLeds, kPlayerLedCycle.size(), kPlayerLedCycleMs};
    case BenchTest::TriggerPulse:
        return SurfacePlan{BenchSurface::TriggerEffects, 1, kStateHoldMs};
    case BenchTest::LampOn:
    case BenchTest::LampPulse:
        return SurfacePlan{BenchSurface::MicLamp, 1, kStateHoldMs};
    case BenchTest::RumbleWeak:
    case BenchTest::RumbleStrong:
    case BenchTest::RumbleBoth:
    case BenchTest::TriggerRumbleLeft:
    case BenchTest::TriggerRumbleRight:
    case BenchTest::TriggerRumbleBoth:
    case BenchTest::TonePlay:
    case BenchTest::HapticsPlay:
    case BenchTest::MicLevel:
    case BenchTest::StickDrift:
    case BenchTest::StickRange:
        return std::nullopt;
    }
    return std::nullopt;
}

inline std::int64_t benchStepAtMs(std::size_t step) {
    return static_cast<std::int64_t>(step) * kBenchStepMs;
}

namespace detail {

inline SurfaceWrite lightbarStep(std::size_t step) {
    SurfaceWrite write{BenchSurface::Lightbar, kIdleSurfaces};
    write.values.lightbar = kLightbarCycle[step % kLightbarCycle.size()];
    return write;
}

inline SurfaceWrite playerLedsStep(std::size_t step) {
    SurfaceWrite write{BenchSurface::PlayerLeds, kIdleSurfaces};
    write.values.playerLeds = kPlayerLedCycle[step % kPlayerLedCycle.size()];
    return write;
}

inline SurfaceWrite triggerPulseStep() {
    SurfaceWrite write{BenchSurface::TriggerEffects, kIdleSurfaces};
    write.values.triggerEffects = rigidTriggerEffects();
    return write;
}

inline SurfaceWrite lampStep(std::uint8_t state) {
    SurfaceWrite write{BenchSurface::MicLamp, kIdleSurfaces};
    write.values.micLamp = state;
    return write;
}

} // namespace detail

// The value step `step` of a surface test writes. A test with no surface writes the idle bar,
// which no caller asks for: surfacePlanFor names the tests that have steps.
inline SurfaceWrite surfaceStepFor(BenchTest test, std::size_t step) {
    switch (test) {
    case BenchTest::LightbarCycle:
        return detail::lightbarStep(step);
    case BenchTest::PlayerLedsCycle:
        return detail::playerLedsStep(step);
    case BenchTest::TriggerPulse:
        return detail::triggerPulseStep();
    case BenchTest::LampOn:
        return detail::lampStep(proto::kMicLedStateOn);
    case BenchTest::LampPulse:
        return detail::lampStep(proto::kMicLedStatePulse);
    case BenchTest::RumbleWeak:
    case BenchTest::RumbleStrong:
    case BenchTest::RumbleBoth:
    case BenchTest::TriggerRumbleLeft:
    case BenchTest::TriggerRumbleRight:
    case BenchTest::TriggerRumbleBoth:
    case BenchTest::TonePlay:
    case BenchTest::HapticsPlay:
    case BenchTest::MicLevel:
    case BenchTest::StickDrift:
    case BenchTest::StickRange:
        return SurfaceWrite{BenchSurface::Lightbar, kIdleSurfaces};
    }
    return SurfaceWrite{BenchSurface::Lightbar, kIdleSurfaces};
}

} // namespace dish::reducer
