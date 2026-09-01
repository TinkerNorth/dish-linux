// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;

namespace {
std::array<std::uint8_t, 4> token4() { return {0x00, 0x07, 0xA1, 0xB2}; }
std::array<std::uint8_t, 32> key32() {
    std::array<std::uint8_t, 32> k{};
    k[0] = 0xAB;
    return k;
}
} // namespace

TEST_CASE("send counter starts at 1 after setConnectionParams", "[satellite][counter]") {
    // Counter 0 is off-spec: the first packet on the wire has to carry 1.
    SatelliteClient c;
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    REQUIRE(c.sendCounter() == 1u);
}

TEST_CASE("setConnectionParams resets reconcile/close state to -1", "[satellite][session]") {
    SatelliteClient c;
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    // -1 is the "no enriched ack / close-notify seen yet" sentinel.
    REQUIRE(c.serverEpoch() == -1);
    REQUIRE(c.serverBitmap() == -1);
    REQUIRE(c.backendAvailable() == -1);
    REQUIRE(c.activeControllerCount() == -1);
    REQUIRE(c.sessionCloseReason() == -1);
    REQUIRE(c.isAlive());
}

TEST_CASE("setConnectionParams starts the latency window empty", "[satellite][latency]") {
    // A re-key must empty the RTT window so the chip only ever reflects the
    // live session.
    SatelliteClient c;
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    const auto snap = c.latencySnapshot();
    REQUIRE(snap.samples == 0);
    REQUIRE(snap.oneWayMs == 0.0);
}

TEST_CASE("a fresh client re-keyed twice keeps the counter at 1", "[satellite][counter]") {
    // Each session PUT rotates token/salt/key, so no counter is reused under a
    // key it already ran under.
    SatelliteClient c;
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    c.setConnectionParams({0x11, 0x22, 0x33, 0x44}, key32(), dish::proto::kProtocolVersion);
    REQUIRE(c.sendCounter() == 1u);
}

TEST_CASE("heartbeat cadence + miss thresholds match the contract", "[satellite][heartbeat]") {
    REQUIRE(SatelliteClient::kHeartbeatIntervalMs == 2000);
    REQUIRE(SatelliteClient::kHeartbeatMissNotResponding == 2);
    REQUIRE(SatelliteClient::kHeartbeatMissMax == 5);
}

TEST_CASE("close + heartbeat handler installation is null-safe", "[satellite][session]") {
    // The handlers fire from the receive thread once live; installing them
    // before any socket exists must still be safe.
    SatelliteClient c;
    c.setHeartbeatAckHandler([](const SatelliteClient::HeartbeatAck&) {});
    c.setCloseHandler([](std::uint8_t) {});
    c.setRumbleHandler([](const SatelliteClient::RumbleMessage&) {});
    c.setLightbarHandler([](const SatelliteClient::LightbarMessage&) {});
    SUCCEED();
}
