// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The support report's edge to a Satellite's connection-diagnostics route:
// only the route's own document is taken, and it is converted value for value,
// whole numbers staying whole.

#include "source/http/ConnectionSnapshotGateway.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QString>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using dish::composer::SnapshotFetchReply;
using dish::diagnostics::Json;
using dish::source::connectionSnapshotFromBody;
using dish::source::ConnectionSnapshotGateway;

namespace {

constexpr const char* kSnapshotBody =
    R"({"format":"satellite-connection-diagnostics.v1","satellite":{"version":"2.3.0",)"
    R"("protocol":[1,4],"uptimeS":3600},"counters":{"scope":"process","rx":{"input":12345}},)"
    R"("rtt":2.5,"grants":null,"ok":true})";

// What the gateway asked the network layer, and the answer it is given.
struct ScriptedFetch {
    QString askedKey;
    int askedTimeoutMs = 0;
    int status = 0;
    bool timedOut = false;
    QByteArray body;

    void operator()(const QString& key, int timeoutMs,
                    const ConnectionSnapshotGateway::RawReply& reply) {
        askedKey = key;
        askedTimeoutMs = timeoutMs;
        reply(status, timedOut, body);
    }
};

} // namespace

TEST_CASE("connection snapshot: the route's own document, value for value",
          "[diagnostics][snapshot]") {
    const std::optional<Json> snapshot = connectionSnapshotFromBody(QByteArray(kSnapshotBody));
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->find("format")->stringValue() == "satellite-connection-diagnostics.v1");
    const Json* satellite = snapshot->find("satellite");
    REQUIRE(satellite != nullptr);
    CHECK(satellite->find("uptimeS")->kind() == Json::Kind::Integer);
    CHECK(satellite->find("uptimeS")->integerValue() == 3600);
    CHECK(satellite->find("protocol")->items().size() == 2);
    CHECK(snapshot->find("counters")->find("rx")->find("input")->integerValue() == 12345);
    CHECK(snapshot->find("rtt")->kind() == Json::Kind::Double);
    CHECK(snapshot->find("rtt")->doubleValue() == 2.5);
    CHECK(snapshot->find("grants")->kind() == Json::Kind::Null);
    CHECK(snapshot->find("ok")->boolValue());
}

TEST_CASE("connection snapshot: anything else is no snapshot", "[diagnostics][snapshot]") {
    CHECK_FALSE(connectionSnapshotFromBody(QByteArray()).has_value());
    CHECK_FALSE(connectionSnapshotFromBody(QByteArray("<html>Satellite</html>")).has_value());
    CHECK_FALSE(connectionSnapshotFromBody(QByteArray("[1,2]")).has_value());
    CHECK_FALSE(
        connectionSnapshotFromBody(QByteArray(R"({"format":"satellite-debug"})")).has_value());
    CHECK_FALSE(connectionSnapshotFromBody(QByteArray(R"({"ok":true})")).has_value());
}

TEST_CASE(
    "connection snapshot: the gateway asks with the two-second deadline and passes the answer",
    "[diagnostics][snapshot]") {
    ScriptedFetch script;
    script.status = 200;
    script.body = QByteArray(kSnapshotBody);
    std::vector<SnapshotFetchReply> replies;
    ConnectionSnapshotGateway gateway([&script](const QString& key, int timeoutMs,
                                                const ConnectionSnapshotGateway::RawReply& reply) {
        script(key, timeoutMs, reply);
    });
    gateway.fetch("sat-a",
                  [&replies](SnapshotFetchReply reply) { replies.push_back(std::move(reply)); });
    CHECK(script.askedKey == QStringLiteral("sat-a"));
    CHECK(script.askedTimeoutMs == 2000);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].status == 200);
    CHECK(replies[0].snapshot.has_value());

    SECTION("a timeout carries no snapshot") {
        script.status = 0;
        script.timedOut = true;
        script.body.clear();
        gateway.fetch(
            "sat-a", [&replies](SnapshotFetchReply reply) { replies.push_back(std::move(reply)); });
        REQUIRE(replies.size() == 2);
        CHECK(replies[1].timedOut);
        CHECK_FALSE(replies[1].snapshot.has_value());
    }
}
