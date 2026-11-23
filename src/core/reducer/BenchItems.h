// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the Diagnostics bench can test, by the contract's names
// (docs/diagnostics-bundle.md, "The bench"), and which of it a pad is offered:
// an item appears only where the pad's path carries it, through the same
// router answer the game's own writes pass (rule 1). No host is in the loop,
// so the audio items follow the pad's own endpoint, not a host's grant.

#pragma once

#include "core/reducer/FeedbackRouting.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dish::reducer {

enum class BenchItem : std::uint8_t {
    Rumble,
    TriggerRumble,
    Lightbar,
    PlayerLeds,
    AdaptiveTriggers,
    MicLamp,
    Speaker,
    Haptics,
    Microphone,
    Sticks,
};
inline constexpr std::size_t kBenchItems = 10;

enum class BenchTest : std::uint8_t {
    RumbleWeak,
    RumbleStrong,
    RumbleBoth,
    TriggerRumbleLeft,
    TriggerRumbleRight,
    TriggerRumbleBoth,
    LightbarCycle,
    PlayerLedsCycle,
    TriggerPulse,
    LampOn,
    LampPulse,
    TonePlay,
    HapticsPlay,
    MicLevel,
    StickDrift,
    StickRange,
};
inline constexpr std::size_t kBenchTests = 16;

struct BenchItemRow {
    BenchItem item;
    // The word the page names the item by.
    std::string_view token;
};

// In BenchItem's order.
inline constexpr std::array<BenchItemRow, kBenchItems> kBenchItemRows{{
    {BenchItem::Rumble, "rumble"},
    {BenchItem::TriggerRumble, "triggerRumble"},
    {BenchItem::Lightbar, "lightbar"},
    {BenchItem::PlayerLeds, "playerLeds"},
    {BenchItem::AdaptiveTriggers, "adaptiveTriggers"},
    {BenchItem::MicLamp, "micLamp"},
    {BenchItem::Speaker, "speaker"},
    {BenchItem::Haptics, "haptics"},
    {BenchItem::Microphone, "microphone"},
    {BenchItem::Sticks, "sticks"},
}};

struct BenchTestRow {
    BenchTest test;
    BenchItem item;
    // The outcome's `test`, `<lane>.<variant>`.
    std::string_view name;
};

// In BenchTest's order.
inline constexpr std::array<BenchTestRow, kBenchTests> kBenchTestRows{{
    {BenchTest::RumbleWeak, BenchItem::Rumble, "rumble.weak"},
    {BenchTest::RumbleStrong, BenchItem::Rumble, "rumble.strong"},
    {BenchTest::RumbleBoth, BenchItem::Rumble, "rumble.both"},
    {BenchTest::TriggerRumbleLeft, BenchItem::TriggerRumble, "triggerRumble.left"},
    {BenchTest::TriggerRumbleRight, BenchItem::TriggerRumble, "triggerRumble.right"},
    {BenchTest::TriggerRumbleBoth, BenchItem::TriggerRumble, "triggerRumble.both"},
    {BenchTest::LightbarCycle, BenchItem::Lightbar, "lightbar.cycle"},
    {BenchTest::PlayerLedsCycle, BenchItem::PlayerLeds, "playerLeds.cycle"},
    {BenchTest::TriggerPulse, BenchItem::AdaptiveTriggers, "trigger.pulse"},
    {BenchTest::LampOn, BenchItem::MicLamp, "lamp.on"},
    {BenchTest::LampPulse, BenchItem::MicLamp, "lamp.pulse"},
    {BenchTest::TonePlay, BenchItem::Speaker, "tone.play"},
    {BenchTest::HapticsPlay, BenchItem::Haptics, "haptics.play"},
    {BenchTest::MicLevel, BenchItem::Microphone, "mic.level"},
    {BenchTest::StickDrift, BenchItem::Sticks, "stick.drift"},
    {BenchTest::StickRange, BenchItem::Sticks, "stick.range"},
}};

inline const BenchTestRow& benchTestRow(BenchTest test) {
    return kBenchTestRows[static_cast<std::size_t>(test)];
}

inline std::string_view benchTestName(BenchTest test) { return benchTestRow(test).name; }

inline BenchItem benchItemOf(BenchTest test) { return benchTestRow(test).item; }

inline std::string_view benchItemToken(BenchItem item) {
    return kBenchItemRows[static_cast<std::size_t>(item)].token;
}

inline std::optional<BenchTest> benchTestFrom(std::string_view name) {
    for (const BenchTestRow& row : kBenchTestRows) {
        if (row.name == name) { return row.test; }
    }
    return std::nullopt;
}

// Rule 1: whether the pad's path carries the item, as the router answers for the game's writes.
// The sticks are every pad's input, so the inspector's two checks are always there.
inline bool benchOffers(const SlotFeedbackInputs& in, BenchItem item) {
    switch (item) {
    case BenchItem::Rumble:
        return slotCarriesFeedback(in, FeedbackKind::Rumble);
    case BenchItem::TriggerRumble:
        return slotCarriesTriggerRumble(in);
    case BenchItem::Lightbar:
        return slotCarriesFeedback(in, FeedbackKind::Lightbar);
    case BenchItem::PlayerLeds:
        return slotCarriesFeedback(in, FeedbackKind::PlayerLeds);
    case BenchItem::AdaptiveTriggers:
        return slotCarriesFeedback(in, FeedbackKind::TriggerEffects);
    case BenchItem::MicLamp:
        return slotCarriesFeedback(in, FeedbackKind::MicLed);
    case BenchItem::Speaker:
        return slotCarriesSpeakerPlayout(in);
    case BenchItem::Haptics:
        return slotCarriesHapticPlayout(in);
    case BenchItem::Microphone:
        return slotCarriesMicCapture(in);
    case BenchItem::Sticks:
        return true;
    }
    return false;
}

} // namespace dish::reducer
