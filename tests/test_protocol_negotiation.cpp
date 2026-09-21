// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Version negotiation, the client half of the contract's "Versioning" section.
//
// The rules worth pinning are the ones a plausible implementation gets wrong:
// the SETTLED version is what the satellite answered rather than what we asked
// for, a 409 whose ranges still overlap is a retry rather than a dead end, and a
// 409 nobody can interpret must not pick a side.

#include "core/model/Protocol.h"
#include "core/reducer/ProtocolNegotiation.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::compatForOutcome;
using dish::reducer::ProtocolCompat;
using dish::reducer::protocolCompatBlocks;
using dish::reducer::ProtocolOutcome;
using dish::reducer::ProtocolVerdict;
using dish::reducer::protocolVerdictTerminal;
using dish::reducer::settleAccepted;
using dish::reducer::settleRejected;

namespace proto = dish::proto;

TEST_CASE("the client offers 3 and still speaks 1", "[protocol][negotiation]") {
    // Guards the rest of this file: every expectation below is written against
    // this range, so a bump has to come here first.
    CHECK(proto::kProtocolVersion == 3);
    CHECK(proto::kProtocolVersionMin == 1);
    CHECK(proto::settledSpeaksV2(2));
    CHECK_FALSE(proto::settledSpeaksV2(1));
}

TEST_CASE("an echo of our own version settles there with no hint", "[protocol][negotiation]") {
    const auto out = settleAccepted(proto::kProtocolVersion);
    CHECK(out.verdict == ProtocolVerdict::Settled);
    CHECK(out.settledVersion == proto::kProtocolVersion);
    CHECK_FALSE(out.satelliteBehind);
}

TEST_CASE("a pre-versioning satellite echoes 1 and the session settles there",
          "[protocol][negotiation]") {
    // The bug this pins: reading the OFFER back instead of the ECHO would send
    // 19-byte POINTER frames to a server that only decodes 16.
    const auto out = settleAccepted(1);
    CHECK(out.verdict == ProtocolVerdict::Settled);
    CHECK(out.settledVersion == 1);
    CHECK(out.satelliteBehind);
    CHECK_FALSE(proto::settledSpeaksV2(out.settledVersion));
}

TEST_CASE("an absent protocolVersion reads as 1", "[protocol][negotiation]") {
    // The DTO substitutes kProtocolVersionMin for an absent field, so this is
    // the same call the manager makes for a body with no version at all.
    const auto out = settleAccepted(proto::kProtocolVersionMin);
    CHECK(out.settledVersion == 1);
    CHECK(out.satelliteBehind);
}

TEST_CASE("an echo below our floor clamps to the floor", "[protocol][negotiation]") {
    // A 0 or negative echo is a malformed body, not an invitation to encode
    // frames no version of this client has.
    for (const int echoed : {0, -1, -7}) {
        const auto out = settleAccepted(echoed);
        CHECK(out.settledVersion == proto::kProtocolVersionMin);
        CHECK(out.satelliteBehind);
    }
}

TEST_CASE("an echo above our version is clamped down, never trusted", "[protocol][negotiation]") {
    // The contract says the server settles on the CLIENT's offer, so this is a
    // server that broke it. Encoding frames we have no encoder for would be the
    // worse of the two failures.
    const auto out = settleAccepted(proto::kProtocolVersion + 5);
    CHECK(out.settledVersion == proto::kProtocolVersion);
    CHECK_FALSE(out.satelliteBehind);
}

TEST_CASE("a 409 whose floor is above us means update the app", "[protocol][negotiation]") {
    const auto out = settleRejected(/*supported=*/proto::kProtocolVersion + 2,
                                    /*supportedMin=*/proto::kProtocolVersion + 1);
    CHECK(out.verdict == ProtocolVerdict::UpdateDish);
    CHECK(protocolVerdictTerminal(out.verdict));
}

TEST_CASE("a 409 whose ceiling is below our floor means update the satellite",
          "[protocol][negotiation]") {
    // Only reachable once this client's floor rises above 1; written now so the
    // rule is pinned before the floor moves.
    const auto out = settleRejected(/*supported=*/proto::kProtocolVersionMin - 1,
                                    /*supportedMin=*/proto::kProtocolVersionMin - 1);
    if constexpr (proto::kProtocolVersionMin > 1) {
        CHECK(out.verdict == ProtocolVerdict::UpdateSatellite);
        CHECK(protocolVerdictTerminal(out.verdict));
    } else {
        // With a floor of 1 the bounds are 0, which is the unusable case: a
        // body that names no range cannot say which end is behind.
        CHECK(out.verdict == ProtocolVerdict::Unusable);
    }
}

TEST_CASE("a 409 with an overlapping range retries at the satellite's ceiling",
          "[protocol][negotiation]") {
    // The case a future client hits against today's satellite: it speaks [1,2],
    // a v3 client offers 3, and 2 is a perfectly good session. Answering
    // "update the satellite" here would strand a user who needs nothing.
    const auto out = settleRejected(/*supported=*/1, /*supportedMin=*/1);
    CHECK(out.verdict == ProtocolVerdict::RetryLower);
    CHECK(out.settledVersion == 1);
    CHECK_FALSE(protocolVerdictTerminal(out.verdict));
}

TEST_CASE("a 409 with no usable bounds picks no side", "[protocol][negotiation]") {
    // Blaming an end from a body that never named one is how a user ends up
    // reinstalling the wrong half.
    CHECK(settleRejected(0, 0).verdict == ProtocolVerdict::Unusable);
    CHECK(settleRejected(2, 0).verdict == ProtocolVerdict::Unusable);
    CHECK(settleRejected(0, 1).verdict == ProtocolVerdict::Unusable);
    CHECK(settleRejected(-1, -1).verdict == ProtocolVerdict::Unusable);
    // Inverted: min above max is not a range.
    CHECK(settleRejected(1, 2).verdict == ProtocolVerdict::Unusable);
    CHECK(protocolVerdictTerminal(ProtocolVerdict::Unusable));
}

TEST_CASE("a settled verdict is never terminal", "[protocol][negotiation]") {
    CHECK_FALSE(protocolVerdictTerminal(ProtocolVerdict::Settled));
}

// ── The compat chip ──────────────────────────────────────────────────────────
// The same five states dish-android's DishProtocol.Compat draws, keyed here on
// the negotiation rather than an advertised number: red only when the session
// cannot open, amber when it opened on an older version, nothing otherwise.

TEST_CASE("compat: a session settled at this build's version is Current", "[protocol][compat]") {
    CHECK(compatForOutcome(settleAccepted(proto::kProtocolVersion)) == ProtocolCompat::Current);
    // A satellite that echoes above us settled on our version too.
    CHECK(compatForOutcome(settleAccepted(proto::kProtocolVersion + 1)) == ProtocolCompat::Current);
}

TEST_CASE("compat: a session settled below this build is the soft satellite hint",
          "[protocol][compat]") {
    const auto c = compatForOutcome(settleAccepted(proto::kProtocolVersionMin));
    CHECK(c == ProtocolCompat::SatelliteUpdateAvailable);
    CHECK_FALSE(protocolCompatBlocks(c));
}

TEST_CASE("compat: a 409 whose floor is above us is Dish update required", "[protocol][compat]") {
    const auto c =
        compatForOutcome(settleRejected(proto::kProtocolVersion + 2, proto::kProtocolVersion + 1));
    CHECK(c == ProtocolCompat::DishUpdateRequired);
    CHECK(protocolCompatBlocks(c));
}

TEST_CASE("compat: a 409 whose ceiling is below our floor is Satellite update required",
          "[protocol][compat]") {
    // Only reachable once kProtocolVersionMin > 1; build the outcome directly so
    // the mapping is pinned regardless of the floor this build ships.
    ProtocolOutcome out;
    out.verdict = ProtocolVerdict::UpdateSatellite;
    const auto c = compatForOutcome(out);
    CHECK(c == ProtocolCompat::SatelliteUpdateRequired);
    CHECK(protocolCompatBlocks(c));
}

TEST_CASE("compat: a retry and an unreadable 409 both say nothing yet", "[protocol][compat]") {
    // Overlapping ranges: the re-offer decides one round trip later.
    CHECK(compatForOutcome(settleRejected(proto::kProtocolVersionMin,
                                          proto::kProtocolVersionMin)) == ProtocolCompat::Unknown);
    // No usable bounds: the chip must not pick a side the wire did not name.
    CHECK(compatForOutcome(settleRejected(0, 0)) == ProtocolCompat::Unknown);
    CHECK_FALSE(protocolCompatBlocks(ProtocolCompat::Unknown));
    CHECK_FALSE(protocolCompatBlocks(ProtocolCompat::Current));
}
