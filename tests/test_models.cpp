// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Models/Models.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>

using namespace dish::models;

TEST_CASE("DiscoveredServer::id encodes ip + udp port", "[models]") {
    DiscoveredServer s;
    s.ip = "192.168.1.42";
    s.udpPort = 9876;
    REQUIRE(s.id() == "wifi:192.168.1.42:9876");
    REQUIRE(s.isValid());
}

TEST_CASE("DiscoveredServer.fromJson defaults missing ports", "[models]") {
    QJsonObject obj{{"name", "satellite-1"}};
    const auto s = DiscoveredServer::fromJson(obj);
    REQUIRE(s.name == "satellite-1");
    REQUIRE(s.ip.isEmpty());
    REQUIRE(s.udpPort == kDefaultUdpPort);
    REQUIRE(s.pairPort == kDefaultPairPort);
    REQUIRE(s.httpPort == kDefaultHttpPort);
}

TEST_CASE("DiscoveredServer.toJson / fromJson round-trip", "[models]") {
    DiscoveredServer in;
    in.name = "kitchen";
    in.ip = "10.0.0.5";
    in.udpPort = 1111;
    in.pairPort = 2222;
    in.httpPort = 3333;
    const auto out = DiscoveredServer::fromJson(in.toJson());
    REQUIRE(out.name == in.name);
    REQUIRE(out.ip == in.ip);
    REQUIRE(out.udpPort == in.udpPort);
    REQUIRE(out.pairPort == in.pairPort);
    REQUIRE(out.httpPort == in.httpPort);
}

TEST_CASE("PairResponse parses ok/error/sharedKey", "[models]") {
    const auto good = PairResponse::fromJson(QJsonObject{{"ok", true}, {"sharedKey", "deadbeef"}});
    REQUIRE(good.ok);
    REQUIRE_FALSE(good.error.has_value());
    REQUIRE(good.sharedKey.has_value());
    REQUIRE(*good.sharedKey == "deadbeef");

    const auto bad = PairResponse::fromJson(QJsonObject{{"ok", false}, {"error", "bad pin"}});
    REQUIRE_FALSE(bad.ok);
    REQUIRE(bad.error.has_value());
    REQUIRE(*bad.error == "bad pin");
}

TEST_CASE("ConnectResponse parses connectionId + token", "[models]") {
    const auto r =
        ConnectResponse::fromJson(QJsonObject{{"connectionId", "abc-123"}, {"token", "tok"}});
    REQUIRE(r.connectionId.has_value());
    REQUIRE(*r.connectionId == "abc-123");
    REQUIRE(r.token.has_value());
    REQUIRE(*r.token == "tok");
    REQUIRE_FALSE(r.error.has_value());
}

TEST_CASE("RememberedWifi round-trips through JSON list", "[models]") {
    RememberedWifi r;
    r.id = "wifi:1.2.3.4:9876";
    r.name = "home";
    r.ip = "1.2.3.4";
    QList<RememberedWifi> list{r};
    const auto arr = rememberedListToJson(list);
    const auto back = rememberedListFromJson(arr);
    REQUIRE(back.size() == 1);
    REQUIRE(back.first().id == r.id);
    REQUIRE(back.first().ip == r.ip);
    REQUIRE(back.first().udpPort == kDefaultUdpPort);
}
