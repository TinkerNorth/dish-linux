// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/model/Protocol.h"
#include "core/reducer/SessionDiagnostics.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using dish::reducer::ackArrived;
using dish::reducer::advertisedCaps;
using dish::reducer::Agreement;
using dish::reducer::bitmapAgreement;
using dish::reducer::bitmapHasIndex;
using dish::reducer::bitmapIndices;
using dish::reducer::epochAgreement;
using dish::reducer::HostBackend;
using dish::reducer::hostBackendOf;
using dish::reducer::hostControllerIndices;
using dish::reducer::kControllerBitmapWidth;
using dish::reducer::Streaming;
using dish::reducer::streamingOf;
using dish::reducer::WireCap;
namespace proto = dish::proto;

namespace {
// What SatelliteClient's ack fields read before the first enriched ack.
constexpr int kNotAcked = -1;
} // namespace

TEST_CASE("an ack field has arrived once it reads zero or more", "[diagnostics][session]") {
    REQUIRE_FALSE(ackArrived(kNotAcked));
    REQUIRE(ackArrived(0));
    REQUIRE(ackArrived(7));
}

TEST_CASE("epoch agreement is unknown before the first enriched ack", "[diagnostics][session]") {
    REQUIRE(epochAgreement(kNotAcked, 4) == Agreement::Unknown);
}

TEST_CASE("epoch agreement is in step while the host is on the applied epoch",
          "[diagnostics][session]") {
    REQUIRE(epochAgreement(4, 4) == Agreement::InStep);
    REQUIRE(epochAgreement(0, 0) == Agreement::InStep);
}

TEST_CASE("epoch agreement diverges once the host moves past the applied epoch",
          "[diagnostics][session]") {
    REQUIRE(epochAgreement(5, 4) == Agreement::Diverged);
}

TEST_CASE("bitmap agreement is unknown before the first enriched ack", "[diagnostics][session]") {
    REQUIRE(bitmapAgreement(kNotAcked, 0x0001) == Agreement::Unknown);
}

TEST_CASE("bitmap agreement is in step when the host runs exactly the confirmed controllers",
          "[diagnostics][session]") {
    REQUIRE(bitmapAgreement(0x0005, 0x0005) == Agreement::InStep);
    REQUIRE(bitmapAgreement(0, 0) == Agreement::InStep);
}

TEST_CASE("bitmap agreement diverges on a controller only one side has", "[diagnostics][session]") {
    // The host runs one this client never confirmed, and misses one it did.
    REQUIRE(bitmapAgreement(0x0003, 0x0001) == Agreement::Diverged);
    REQUIRE(bitmapAgreement(0x0001, 0x0003) == Agreement::Diverged);
}

TEST_CASE("bitmap indices name the set bits lowest first", "[diagnostics][session]") {
    REQUIRE(bitmapIndices(0x8005) == std::vector<int>{0, 2, 15});
    REQUIRE(bitmapIndices(0).empty());
}

TEST_CASE("the host reports no controllers before its first enriched ack",
          "[diagnostics][session]") {
    REQUIRE(hostControllerIndices(kNotAcked).empty());
}

TEST_CASE("the host's controllers are the ones its bitmap names", "[diagnostics][session]") {
    REQUIRE(hostControllerIndices(0x0005) == std::vector<int>{0, 2});
    REQUIRE(hostControllerIndices(0).empty());
}

TEST_CASE("a bitmap never holds an index outside its sixteen bits", "[diagnostics][session]") {
    REQUIRE(bitmapHasIndex(0xFFFF, 0));
    REQUIRE(bitmapHasIndex(0xFFFF, kControllerBitmapWidth - 1));
    REQUIRE_FALSE(bitmapHasIndex(0xFFFF, -1));
    REQUIRE_FALSE(bitmapHasIndex(0xFFFF, kControllerBitmapWidth));
    REQUIRE_FALSE(bitmapHasIndex(0x0002, 0));
}

TEST_CASE("streaming is unknown before the first enriched ack", "[diagnostics][session]") {
    REQUIRE(streamingOf(kNotAcked, 0) == Streaming::Unknown);
}

TEST_CASE("streaming follows the controller's bit in the host's bitmap", "[diagnostics][session]") {
    REQUIRE(streamingOf(0x0004, 2) == Streaming::Yes);
    REQUIRE(streamingOf(0x0004, 0) == Streaming::No);
    REQUIRE(streamingOf(0, 0) == Streaming::No);
}

TEST_CASE("the host backend reads unknown, available or unavailable off the ack byte",
          "[diagnostics][session]") {
    REQUIRE(hostBackendOf(kNotAcked) == HostBackend::Unknown);
    REQUIRE(hostBackendOf(1) == HostBackend::Available);
    REQUIRE(hostBackendOf(0) == HostBackend::Unavailable);
}

TEST_CASE("an empty caps word advertises nothing", "[diagnostics][session]") {
    REQUIRE(advertisedCaps(0).empty());
}

TEST_CASE("every advertised cap bit names its capability, in wire order",
          "[diagnostics][session]") {
    const std::uint16_t all = proto::kCapAnalogTriggers | proto::kCapRumble | proto::kCapMotion |
                              proto::kCapLightbar | proto::kCapTriggerEffects |
                              proto::kCapPlayerLeds | proto::kCapMic | proto::kCapSpeaker |
                              proto::kCapHapticAudio;
    REQUIRE(advertisedCaps(all) ==
            std::vector<WireCap>{WireCap::AnalogTriggers, WireCap::Rumble, WireCap::Motion,
                                 WireCap::Lightbar, WireCap::TriggerEffects, WireCap::PlayerLeds,
                                 WireCap::Mic, WireCap::Speaker, WireCap::HapticAudio});
}

TEST_CASE("a descriptor's caps advertise only the bits it set", "[diagnostics][session]") {
    const std::uint16_t caps = proto::kCapAnalogTriggers | proto::kCapMotion | proto::kCapMic;
    REQUIRE(advertisedCaps(caps) ==
            std::vector<WireCap>{WireCap::AnalogTriggers, WireCap::Motion, WireCap::Mic});
}
