// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Models/Models.h"
#include "Network/PairingClient.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonObject>

using dish::models::PairResponse;
using dish::net::PairingClient;

namespace {

template <typename Arm> bool holds(const PairingClient::Outcome& o) {
    return std::holds_alternative<Arm>(o);
}

} // namespace

TEST_CASE("classify: ok + sharedKey returns Success", "[pairing]") {
    PairResponse r;
    r.ok = true;
    r.sharedKey = QStringLiteral("abcd");
    r.reachable = true;
    const auto o = PairingClient::classify(r);
    REQUIRE(holds<PairingClient::Success>(o));
    REQUIRE(std::get<PairingClient::Success>(o).sharedKeyHex == "abcd");
}

TEST_CASE("classify: reachable but !ok returns AuthRequired", "[pairing]") {
    PairResponse r;
    r.ok = false;
    r.reachable = true;
    r.error = QStringLiteral("bad pin");
    REQUIRE(holds<PairingClient::AuthRequired>(PairingClient::classify(r)));
}

TEST_CASE("classify: unreachable surfaces network error", "[pairing]") {
    PairResponse r;
    r.ok = false;
    r.reachable = false;
    r.error = QStringLiteral("connect timeout");
    const auto o = PairingClient::classify(r);
    REQUIRE(holds<PairingClient::Unreachable>(o));
    REQUIRE(std::get<PairingClient::Unreachable>(o).message == "connect timeout");
}

TEST_CASE("classify: unreachable without error falls back to default", "[pairing]") {
    PairResponse r;
    r.ok = false;
    r.reachable = false;
    const auto o = PairingClient::classify(r);
    REQUIRE(holds<PairingClient::Unreachable>(o));
    REQUIRE(std::get<PairingClient::Unreachable>(o).message == "Server unreachable");
}

TEST_CASE("classify: ok but empty sharedKey is AuthRequired, not Success", "[pairing]") {
    // Defensive: a server that says ok=true but forgets to send a key should
    // fall through to AuthRequired (we did reach it), never Success. Caching
    // an empty string as the shared key would silently break every
    // subsequent reconnect.
    PairResponse r;
    r.ok = true;
    r.sharedKey = QStringLiteral("");
    r.reachable = true;
    REQUIRE(holds<PairingClient::AuthRequired>(PairingClient::classify(r)));
}

TEST_CASE("classify: ok with no sharedKey is AuthRequired", "[pairing]") {
    PairResponse r;
    r.ok = true;
    r.sharedKey.reset();
    r.reachable = true;
    REQUIRE(holds<PairingClient::AuthRequired>(PairingClient::classify(r)));
}

TEST_CASE("PairResponse::fromJson sets reachable=true on a parsed body", "[pairing]") {
    // The wire never carries `reachable` — it's set client-side by fromJson
    // (success path) or by the synthesised error helpers in PairingClient
    // (network-error paths). Pin both branches.
    const QJsonObject body{{"ok", true}, {"sharedKey", "deadbeef"}};
    const auto r = PairResponse::fromJson(body);
    REQUIRE(r.ok);
    REQUIRE(r.reachable);
    REQUIRE(r.sharedKey.has_value());
    REQUIRE(*r.sharedKey == "deadbeef");
}

TEST_CASE("PairResponse default-constructed is reachable=false", "[pairing]") {
    PairResponse r;
    REQUIRE_FALSE(r.reachable);
}
