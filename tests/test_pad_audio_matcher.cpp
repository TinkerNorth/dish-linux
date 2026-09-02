// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pad-to-endpoint matcher (core/audio/PadAudioMatcher.h). The one rule
// under test is CONSERVATISM: the only evidence linking an audio endpoint to a
// USB pad is a product string inside a device name, so every ambiguity must
// resolve to "no route" — routing a slot to the WRONG pad's microphone is the
// failure none of these cases may allow. The fixture names use the Windows
// wrapping ("Speakers (Wireless Controller)"); pipewire's are flatter, and the
// containment rule covers both, which is part of what these cases pin.

#include "core/audio/PadAudioMatcher.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using dish::audio::AudioPadCandidate;
using dish::audio::padAudioKey;
using dish::audio::PadAudioRoute;
using dish::audio::resolvePadAudioRoutes;

namespace {

AudioPadCandidate dualSense(int pid = 0x0CE6, const std::string& name = "Wireless Controller") {
    return AudioPadCandidate{0x054C, pid, name, /*hasAudioFunction=*/true};
}

const std::vector<std::string> kNoDevices;

} // namespace

TEST_CASE("one pad, one endpoint pair: both directions route by name", "[audio][matcher]") {
    const auto routes = resolvePadAudioRoutes(
        {dualSense()}, {"Headset Microphone (Wireless Controller)", "Microphone (USB Webcam)"},
        {"Speakers (Wireless Controller)", "Speakers (Realtek Audio)"});
    REQUIRE(routes.size() == 1U);
    const auto& route = routes.at(padAudioKey(0x054C, 0x0CE6));
    CHECK(route.microphone);
    CHECK(route.speaker);
    // The ORIGINAL device names, verbatim: they are the SDL open keys.
    CHECK(route.captureDeviceName == "Headset Microphone (Wireless Controller)");
    CHECK(route.playbackDeviceName == "Speakers (Wireless Controller)");
}

TEST_CASE("the directions resolve independently", "[audio][matcher]") {
    // Only a speaker endpoint enumerated (a driver that hides the capture
    // side): the pad keeps its speaker and simply has no microphone.
    const auto routes =
        resolvePadAudioRoutes({dualSense()}, kNoDevices, {"Speakers (Wireless Controller)"});
    REQUIRE(routes.size() == 1U);
    const auto& route = routes.at(padAudioKey(0x054C, 0x0CE6));
    CHECK_FALSE(route.microphone);
    CHECK(route.captureDeviceName.empty()); // no flag without a name, no name without a flag
    CHECK(route.speaker);
}

TEST_CASE("no matching endpoint at all publishes nothing", "[audio][matcher]") {
    const auto routes = resolvePadAudioRoutes({dualSense()}, {"Microphone (USB Webcam)"},
                                              {"Speakers (Realtek Audio)"});
    CHECK(routes.empty());
}

TEST_CASE("zero pads or zero endpoints is simply empty", "[audio][matcher]") {
    CHECK(resolvePadAudioRoutes({}, {"Speakers (Wireless Controller)"}, {}).empty());
    CHECK(resolvePadAudioRoutes({dualSense()}, kNoDevices, kNoDevices).empty());
    CHECK(resolvePadAudioRoutes({}, kNoDevices, kNoDevices).empty());
}

TEST_CASE("two pads with the same product string disqualify each other", "[audio][matcher]") {
    // Two DualSenses — or a DualSense next to a DS4, which shares the string —
    // are indistinguishable by name. Neither may route, however many matching
    // endpoints exist.
    const auto routes = resolvePadAudioRoutes({dualSense(0x0CE6), dualSense(0x09CC)},
                                              {"Headset Microphone (Wireless Controller)"},
                                              {"Speakers (Wireless Controller)"});
    CHECK(routes.empty());
}

TEST_CASE("two endpoints answering one pad in one direction cancel that direction",
          "[audio][matcher]") {
    // Two identical dongles enumerate identical friendly names; picking either
    // would be a coin toss with someone's audio.
    const auto routes = resolvePadAudioRoutes(
        {dualSense()},
        {"Headset Microphone (Wireless Controller)", "Headset Microphone (Wireless Controller)"},
        {"Speakers (Wireless Controller)"});
    REQUIRE(routes.size() == 1U);
    const auto& route = routes.at(padAudioKey(0x054C, 0x0CE6));
    CHECK_FALSE(route.microphone); // ambiguous direction: nothing
    CHECK(route.speaker);          // the clean direction survives
}

TEST_CASE("an endpoint naming two different pads belongs to nobody", "[audio][matcher]") {
    // "DualSense Wireless Controller" contains BOTH pad names when one pad's
    // string is a substring of the other's. That endpoint is evidence for
    // neither.
    const auto routes =
        resolvePadAudioRoutes({dualSense(0x0CE6, "Wireless Controller"),
                               dualSense(0x0DF2, "DualSense Wireless Controller")},
                              {"Headset Microphone (DualSense Wireless Controller)"}, kNoDevices);
    // The longer-named pad still cannot claim it either: the endpoint matched
    // two pad names, so it was discarded before ownership was decided.
    CHECK(routes.empty());
}

TEST_CASE("a family without a USB audio function never routes", "[audio][matcher]") {
    // A name alone must not let an unrelated audio dongle lend its endpoints
    // to a pad that has none.
    AudioPadCandidate switchPro{0x057E, 0x2009, "Pro Controller", /*hasAudioFunction=*/false};
    const auto routes = resolvePadAudioRoutes({switchPro}, {"Microphone (Pro Controller)"},
                                              {"Speakers (Pro Controller)"});
    CHECK(routes.empty());
}

TEST_CASE("blank and whitespace product strings are not candidates", "[audio][matcher]") {
    const auto routes =
        resolvePadAudioRoutes({dualSense(0x0CE6, ""), dualSense(0x0DF2, "   ")},
                              {"Headset Microphone (Wireless Controller)"}, kNoDevices);
    CHECK(routes.empty());
}

TEST_CASE("matching is trimmed and ASCII-case-insensitive", "[audio][matcher]") {
    const auto routes =
        resolvePadAudioRoutes({dualSense(0x0CE6, "  Wireless Controller  ")},
                              {"Headset Microphone (WIRELESS CONTROLLER)"}, kNoDevices);
    REQUIRE(routes.size() == 1U);
    CHECK(routes.at(padAudioKey(0x054C, 0x0CE6)).microphone);
}

TEST_CASE("an unrelated endpoint does not disturb the match", "[audio][matcher]") {
    const auto routes = resolvePadAudioRoutes(
        {dualSense()}, {"Microphone (USB Webcam)", "Headset Microphone (Wireless Controller)", ""},
        {"Speakers (Realtek Audio)", "Speakers (Wireless Controller)"});
    REQUIRE(routes.size() == 1U);
    const auto& route = routes.at(padAudioKey(0x054C, 0x0CE6));
    CHECK(route.microphone);
    CHECK(route.speaker);
}

TEST_CASE("two distinct pads route independently", "[audio][matcher]") {
    const auto routes = resolvePadAudioRoutes(
        {dualSense(0x0CE6, "Wireless Controller"), dualSense(0x0DF2, "DualSense Edge")},
        {"Headset Microphone (Wireless Controller)", "Headset Microphone (DualSense Edge)"},
        {"Speakers (DualSense Edge)"});
    REQUIRE(routes.size() == 2U);
    CHECK(routes.at(padAudioKey(0x054C, 0x0CE6)).microphone);
    CHECK_FALSE(routes.at(padAudioKey(0x054C, 0x0CE6)).speaker);
    CHECK(routes.at(padAudioKey(0x054C, 0x0DF2)).microphone);
    CHECK(routes.at(padAudioKey(0x054C, 0x0DF2)).speaker);
}
