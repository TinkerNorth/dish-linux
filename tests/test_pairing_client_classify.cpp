// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Protocol-1 PIN paths always answer HTTP 200, so classify() keys off ok/pending
// rather than the status. The one exception is 409, which marks protocol skew.

#include "Models/Models.h"
#include "Network/PairingClient.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QJsonObject>
#include <QString>

using dish::models::PairResponse;
using dish::net::PairingClient;

namespace {

template <typename Arm> bool holds(const PairingClient::Outcome& o) {
    return std::holds_alternative<Arm>(o);
}

PairResponse reply200(bool ok, bool pending, const QString& key) {
    PairResponse r;
    r.httpStatus = 200;
    r.reachable = true;
    r.ok = ok;
    r.pending = pending;
    if (!key.isEmpty()) { r.sharedKey = key; }
    return r;
}

} // namespace

TEST_CASE("classify: Path-A ok + sharedKey -> Success", "[pairing]") {
    const auto o = PairingClient::classify(reply200(true, false, "abcd"));
    REQUIRE(holds<PairingClient::Success>(o));
    REQUIRE(std::get<PairingClient::Success>(o).sharedKeyHex == "abcd");
}

TEST_CASE("classify: Path-B pending -> Pending", "[pairing]") {
    REQUIRE(holds<PairingClient::Pending>(PairingClient::classify(reply200(false, true, ""))));
}

TEST_CASE("classify: reachable, no key, not pending -> AuthRequired", "[pairing]") {
    PairResponse r = reply200(false, false, "");
    r.error = QStringLiteral("invalid or expired PIN");
    REQUIRE(holds<PairingClient::AuthRequired>(PairingClient::classify(r)));
}

TEST_CASE("classify: ok=true but empty sharedKey is AuthRequired, not Success", "[pairing]") {
    // Defensive: caching an empty key would silently break every reconnect.
    REQUIRE(holds<PairingClient::AuthRequired>(PairingClient::classify(reply200(true, false, ""))));
}

TEST_CASE("classify: 409 -> VersionMismatch (terminal)", "[pairing]") {
    PairResponse r;
    r.httpStatus = 409;
    r.reachable = true;
    REQUIRE(holds<PairingClient::VersionMismatch>(PairingClient::classify(r)));
}

TEST_CASE("classify: unreachable surfaces a network message", "[pairing]") {
    PairResponse r;
    r.httpStatus = 0;
    r.reachable = false;
    r.error = QStringLiteral("connect timeout");
    const auto o = PairingClient::classify(r);
    REQUIRE(holds<PairingClient::Unreachable>(o));
    REQUIRE(std::get<PairingClient::Unreachable>(o).message == "connect timeout");
}

TEST_CASE("classify: unreachable without error falls back to default", "[pairing]") {
    PairResponse r;
    r.httpStatus = 0;
    r.reachable = false;
    const auto o = PairingClient::classify(r);
    REQUIRE(holds<PairingClient::Unreachable>(o));
    // Pin against the same translate() call the production code makes, so this
    // stays green under every bundled translator.
    REQUIRE(std::get<PairingClient::Unreachable>(o).message ==
            QCoreApplication::translate("dish::net::PairingClient", "Server unreachable"));
}

TEST_CASE("PairResponse::fromJson sets reachable=true on a parsed body", "[pairing]") {
    const auto r = PairResponse::fromJson(QJsonObject{{"ok", true}, {"sharedKey", "deadbeef"}});
    REQUIRE(r.ok);
    REQUIRE(r.reachable);
    REQUIRE(*r.sharedKey == "deadbeef");
}
