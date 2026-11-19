// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The refusal ledger (core/audio/AudioRefusal.h): a refused open stands against
// the endpoint it was refused at and nothing else, is the slot's and the lane's
// alone, and leaves with the device that earned it.

#include "core/audio/AudioRefusal.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using dish::audio::AudioEndpoint;
using dish::audio::AudioLane;
using dish::audio::AudioRefusalLedger;
using dish::audio::RefusedSlots;

namespace {

const std::string kSlot = "usb:054c:0ce6";
const std::string kOtherSlot = "sdl:2";
const AudioEndpoint kDualSense{"Wireless Controller Analog Surround 4.0", 4};
const AudioEndpoint kDualSenseStereo{"Wireless Controller Analog Surround 4.0", 2};
const AudioEndpoint kHeadset{"Wireless Controller Mono", 1};
const AudioEndpoint kOtherDevice{"Built-in Audio Analog Stereo", 2};

} // namespace

TEST_CASE("a fresh ledger holds no refusals", "[audio][refusal]") {
    const AudioRefusalLedger ledger;
    CHECK(ledger.empty());
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Speaker, kDualSense));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Haptic, kDualSense));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Mic, kHeadset));
}

TEST_CASE("a recorded refusal stands at its endpoint and at no other", "[audio][refusal]") {
    // The same device at another width is a re-enumerated endpoint, and another device is
    // another device: both are attempts nobody has made yet.
    AudioRefusalLedger ledger;
    CHECK(ledger.record(kSlot, AudioLane::Speaker, kDualSense));
    CHECK_FALSE(ledger.empty());
    CHECK(ledger.stands(kSlot, AudioLane::Speaker, kDualSense));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Speaker, kDualSenseStereo));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Speaker, kOtherDevice));
}

TEST_CASE("a refusal is the slot's and the lane's alone", "[audio][refusal]") {
    AudioRefusalLedger ledger;
    ledger.record(kSlot, AudioLane::Speaker, kDualSense);
    CHECK(ledger.stands(kSlot, AudioLane::Speaker, kDualSense));
    // The haptic lane opens the same endpoint as its own stream, and that stream was not refused.
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Haptic, kDualSense));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Mic, kDualSense));
    CHECK_FALSE(ledger.stands(kOtherSlot, AudioLane::Speaker, kDualSense));
}

TEST_CASE("a newer refusal for the same lane replaces the older", "[audio][refusal]") {
    // One standing refusal per lane: the endpoint tried last is the one that stands, and a
    // refusal already on record is not news.
    AudioRefusalLedger ledger;
    CHECK(ledger.record(kSlot, AudioLane::Speaker, kDualSenseStereo));
    CHECK_FALSE(ledger.record(kSlot, AudioLane::Speaker, kDualSenseStereo));
    CHECK(ledger.record(kSlot, AudioLane::Speaker, kDualSense));
    CHECK(ledger.stands(kSlot, AudioLane::Speaker, kDualSense));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Speaker, kDualSenseStereo));
}

TEST_CASE("lifting a refusal spends it, and lifting nothing is nothing", "[audio][refusal]") {
    AudioRefusalLedger ledger;
    ledger.record(kSlot, AudioLane::Mic, kHeadset);
    ledger.record(kSlot, AudioLane::Speaker, kDualSense);
    CHECK(ledger.lift(kSlot, AudioLane::Mic));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Mic, kHeadset));
    // The other lane's refusal is untouched.
    CHECK(ledger.stands(kSlot, AudioLane::Speaker, kDualSense));
    CHECK_FALSE(ledger.lift(kSlot, AudioLane::Mic));
    CHECK_FALSE(ledger.lift(kOtherSlot, AudioLane::Speaker));
}

TEST_CASE("a departed device takes its refusals with it and names the slots that lost one",
          "[audio][refusal]") {
    // A replugged pad enumerates afresh, so every refusal its old enumeration earned is spent,
    // whichever lane and whichever slot; a device still present keeps its own.
    AudioRefusalLedger ledger;
    ledger.record(kSlot, AudioLane::Speaker, kDualSense);
    ledger.record(kSlot, AudioLane::Haptic, kDualSense);
    ledger.record(kSlot, AudioLane::Mic, kHeadset);
    ledger.record(kOtherSlot, AudioLane::Speaker, kOtherDevice);

    const RefusedSlots lapsed = ledger.pruneAbsent({kHeadset.deviceName, kOtherDevice.deviceName});
    CHECK(lapsed == RefusedSlots{kSlot});
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Speaker, kDualSense));
    CHECK_FALSE(ledger.stands(kSlot, AudioLane::Haptic, kDualSense));
    CHECK(ledger.stands(kSlot, AudioLane::Mic, kHeadset));
    CHECK(ledger.stands(kOtherSlot, AudioLane::Speaker, kOtherDevice));

    // Nothing departed: nothing lapses, and nobody is named.
    CHECK(ledger.pruneAbsent({kHeadset.deviceName, kOtherDevice.deviceName}).empty());
    // Everything departed: every slot is named once, whatever it held.
    const RefusedSlots all = ledger.pruneAbsent({});
    CHECK(all == RefusedSlots{kSlot, kOtherSlot});
    CHECK(ledger.empty());
}
