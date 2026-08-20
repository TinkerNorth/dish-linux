// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/SatelliteClient.h"
#include "core/model/Protocol.h"
#include "core/reducer/CloseNotify.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;
namespace proto = dish::proto;
namespace reducer = dish::reducer;

TEST_CASE("kept opcode constants match the protocol-1 values", "[udp][opcodes]") {
    REQUIRE(SatelliteClient::kMsgInput == 0x0001);
    REQUIRE(SatelliteClient::kMsgHeartbeatPing == 0x0002);
    REQUIRE(SatelliteClient::kMsgHeartbeatAck == 0x0003);
    REQUIRE(SatelliteClient::kMsgRumble == 0x0009);
    REQUIRE(SatelliteClient::kMsgMotion == 0x000A);
    REQUIRE(SatelliteClient::kMsgBattery == 0x000B);
    REQUIRE(SatelliteClient::kMsgTouchpad == 0x000C);
    REQUIRE(SatelliteClient::kMsgLightbar == 0x000D);
    REQUIRE(SatelliteClient::kMsgSessionClose == 0x000F);
}

// 0x0004..0x0008 and 0x000E were the topology opcodes, now removed. A test
// cannot name a member that no longer compiles, so re-introducing one instead
// forces an edit to this list, which surfaces the regression in review.
TEST_CASE("deleted topology opcodes are absent from the opcode set", "[udp][opcodes]") {
    constexpr std::array<std::uint16_t, 9> kept = {
        SatelliteClient::kMsgInput,        SatelliteClient::kMsgHeartbeatPing,
        SatelliteClient::kMsgHeartbeatAck, SatelliteClient::kMsgRumble,
        SatelliteClient::kMsgMotion,       SatelliteClient::kMsgBattery,
        SatelliteClient::kMsgTouchpad,     SatelliteClient::kMsgLightbar,
        SatelliteClient::kMsgSessionClose};
    for (auto op : kept) {
        const bool isDeletedTopologyOpcode = op == 0x0004 || op == 0x0005 || op == 0x0006 ||
                                             op == 0x0007 || op == 0x0008 || op == 0x000E;
        REQUIRE_FALSE(isDeletedTopologyOpcode);
    }
}

TEST_CASE("parseHeartbeatAck decodes backend/count/epoch/bitmap", "[udp][heartbeat]") {
    // backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // activeBitmap(u16 BE). epoch 0x0102 = 258; bitmap 0x0005 = controllers 0,2.
    const std::uint8_t payload[6] = {0x01, 0x03, 0x01, 0x02, 0x00, 0x05};
    const auto ack = SatelliteClient::parseHeartbeatAck(payload, sizeof(payload));
    REQUIRE(ack.has_value());
    REQUIRE(ack->backendAvailable);
    REQUIRE(ack->totalActiveControllers == 3);
    REQUIRE(ack->epoch == 0x0102);
    REQUIRE(ack->activeBitmap == 0x0005);
}

TEST_CASE("parseHeartbeatAck reads backendAvailable=false", "[udp][heartbeat]") {
    const std::uint8_t payload[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto ack = SatelliteClient::parseHeartbeatAck(payload, sizeof(payload));
    REQUIRE(ack.has_value());
    REQUIRE_FALSE(ack->backendAvailable);
    REQUIRE(ack->totalActiveControllers == 0);
    REQUIRE(ack->epoch == 0);
    REQUIRE(ack->activeBitmap == 0);
}

TEST_CASE("parseHeartbeatAck rejects a bare (pre-protocol-1) ack", "[udp][heartbeat]") {
    // nullopt still counts as liveness; only the reconcile fields go unset.
    const std::uint8_t shortPayload[3] = {0x01, 0x00, 0x00};
    REQUIRE_FALSE(SatelliteClient::parseHeartbeatAck(shortPayload, sizeof(shortPayload)));
    REQUIRE_FALSE(SatelliteClient::parseHeartbeatAck(nullptr, 0));
    REQUIRE(proto::kHeartbeatAckPayloadBytes == 6);
}

TEST_CASE("parseHeartbeatAck epoch/bitmap are big-endian", "[udp][heartbeat]") {
    const std::uint8_t payload[6] = {0x01, 0x01, 0xAB, 0xCD, 0x80, 0x01};
    const auto ack = SatelliteClient::parseHeartbeatAck(payload, sizeof(payload));
    REQUIRE(ack->epoch == 0xABCD);        // BE: high byte first
    REQUIRE(ack->activeBitmap == 0x8001); // controllers 0 and 15
}

TEST_CASE("close-notify reason maps to the right teardown action", "[udp][close]") {
    using reducer::CloseAction;
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonUnpaired) ==
            CloseAction::DropKeyRePair);
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonReplaced) == CloseAction::StayDown);
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonShutdown) ==
            CloseAction::RetryBackoff);
    REQUIRE(reducer::closeActionForReason(proto::kCloseReasonKicked) == CloseAction::RetryBackoff);
}

TEST_CASE("close-notify reason bytes match the contract", "[udp][close]") {
    REQUIRE(proto::kCloseReasonShutdown == 0);
    REQUIRE(proto::kCloseReasonKicked == 1);
    REQUIRE(proto::kCloseReasonReplaced == 2);
    REQUIRE(proto::kCloseReasonUnpaired == 3);
    REQUIRE(reducer::closeReasonName(proto::kCloseReasonUnpaired) == "unpaired");
    REQUIRE(reducer::closeReasonName(proto::kCloseReasonKicked) == "kicked");
}

TEST_CASE("parseRumbleMessage decodes the 7-byte BE payload", "[udp][rumble]") {
    // ctrlIdx(1) strong(2 BE) weak(2 BE) durMs(2 BE).
    const std::uint8_t payload[7] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x01, 0xF4};
    const auto rm = SatelliteClient::parseRumbleMessage(payload, sizeof(payload));
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 2);
    REQUIRE(rm->strongMagnitude == 0x1234);
    REQUIRE(rm->weakMagnitude == 0x5678);
    REQUIRE(rm->durationMs == 500);
}

TEST_CASE("parseLightbarMessage decodes ctrlIdx + rgb", "[udp][lightbar]") {
    const std::uint8_t payload[4] = {0x03, 0xFF, 0x80, 0x00};
    const auto lm = SatelliteClient::parseLightbarMessage(payload, sizeof(payload));
    REQUIRE(lm.has_value());
    REQUIRE(lm->controllerIndex == 3);
    REQUIRE(lm->r == 0xFF);
    REQUIRE(lm->g == 0x80);
    REQUIRE(lm->b == 0x00);
}
