// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/SatelliteClient.h"
#include "ServerPacket.h"
#include "core/model/Protocol.h"
#include "satellite_client_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

using dish::net::SatelliteClient;
using dish::net::SatelliteClientTestAccess;
using dish::reducer::LatencySummary;

namespace {
std::array<std::uint8_t, 4> token4() { return {0x00, 0x07, 0xA1, 0xB2}; }
std::array<std::uint8_t, 32> key32() {
    std::array<std::uint8_t, 32> k{};
    k[0] = 0xAB;
    return k;
}

// The ping's age when its ack is fed in, so the round trip is at least this.
constexpr std::int64_t kPingAgeUs = 12'000;
constexpr double kPingAgeMs = 12.0;

std::int64_t steadyNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// An enriched ack: backend up, one controller, epoch 1, controller 0 active.
std::vector<std::uint8_t> ackBody() { return {0x01, 0x01, 0x00, 0x01, 0x00, 0x01}; }

void ackAgedPing(SatelliteClient& c, std::uint32_t counter) {
    SatelliteClientTestAccess::armPing(c, steadyNowUs() - kPingAgeUs);
    const auto ack = dish::test::sealServerPacket(token4(), key32(), dish::proto::kMsgHeartbeatAck,
                                                  ackBody(), counter);
    REQUIRE_FALSE(ack.empty());
    SatelliteClientTestAccess::processIncoming(c, ack.data(), ack.size());
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
    REQUIRE(c.latencySummary() == LatencySummary{});
}

TEST_CASE("an acked ping lands its round trip in the latency summary", "[satellite][latency]") {
    SatelliteClient c;
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    ackAgedPing(c, 1);
    const LatencySummary summary = c.latencySummary();
    REQUIRE(summary.samples == 1);
    REQUIRE(summary.rttP50Ms >= kPingAgeMs);
    REQUIRE(summary.rttP99Ms == summary.rttP50Ms);
}

TEST_CASE("a re-key empties the latency summary", "[satellite][latency]") {
    SatelliteClient c;
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    ackAgedPing(c, 1);
    c.setConnectionParams(token4(), key32(), dish::proto::kProtocolVersion);
    REQUIRE(c.latencySummary() == LatencySummary{});
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

TEST_CASE("openSocket refuses a port outside 1..65535 rather than truncate it",
          "[satellite][session]") {
    // htons of a 16-bit cast would turn 65536 into 0 and 65537 into 1: a socket keyed at a port
    // nobody asked for.
    SatelliteClient zero;
    CHECK_FALSE(zero.openSocket("127.0.0.1", 0));
    CHECK_FALSE(zero.isOpen());
    SatelliteClient negative;
    CHECK_FALSE(negative.openSocket("127.0.0.1", -1));
    SatelliteClient tooHigh;
    CHECK_FALSE(tooHigh.openSocket("127.0.0.1", 65536));

    SatelliteClient lowest;
    CHECK(lowest.openSocket("127.0.0.1", 1));
    SatelliteClient highest;
    CHECK(highest.openSocket("127.0.0.1", 65535));
}
