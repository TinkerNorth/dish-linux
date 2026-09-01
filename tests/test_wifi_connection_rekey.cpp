// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Contract §Crypto: a client SHOULD re-PUT once its send counter crosses
// 0xF0000000. The alive tick is driven through the test seam, so no timer waits.

#include "Network/SatelliteClient.h"
#include "Network/WifiConnection.h"
#include "core/reducer/Reconcile.h"
#include "satellite_client_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <memory>

namespace dish::net {

// Definition of the test-only friend seam declared in WifiConnection.h.
class WifiConnectionTestAccess {
  public:
    static void tick(WifiConnection& conn) { conn.onAliveTick(); }
};

} // namespace dish::net

using dish::net::SatelliteClient;
using dish::net::SatelliteClientTestAccess;
using dish::net::WifiConnection;
using dish::net::WifiConnectionTestAccess;

namespace {

int bindLoopback(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { return -1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    socklen_t len = sizeof(addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

std::array<std::uint8_t, 32> key(std::uint8_t fill) {
    std::array<std::uint8_t, 32> k{};
    k.fill(fill);
    return k;
}

dish::models::DiscoveredServer server() {
    dish::models::DiscoveredServer s;
    s.machineId = QStringLiteral("m1");
    s.ip = QStringLiteral("127.0.0.1");
    s.name = QStringLiteral("Sat");
    return s;
}

} // namespace

TEST_CASE("alive tick fires the rekey callback once per threshold approach and re-arms after "
          "landing",
          "[rekey]") {
    std::uint16_t port = 0;
    const int fd = bindLoopback(port);
    REQUIRE(fd >= 0);

    auto client = std::make_shared<SatelliteClient>();
    REQUIRE(client->openSocket("127.0.0.1", port));
    client->setConnectionParams({0x11, 0x22, 0x33, 0x44}, key(0xA5), dish::proto::kProtocolVersion);

    WifiConnection conn(WifiConnection::idFor(server()), server());
    int rekeyCalls = 0;
    conn.markConnecting();
    conn.markConnected(
        client, QStringLiteral("conn_1"), /*epoch=*/0, /*mouseControlGranted=*/false,
        /*onDead=*/[] {}, /*onClose=*/[](std::uint8_t) {}, /*onReconcile=*/[] {},
        /*onRekey=*/[&rekeyCalls] { rekeyCalls++; });

    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 0);

    // Once per approach, not once per tick.
    SatelliteClientTestAccess::seedSendCounter(*client, dish::reducer::kCounterRepushThreshold);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 1);
    WifiConnectionTestAccess::tick(conn);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 1);

    // Installing a fresh token/key is what the manager's rekey does: the counter
    // restarts under the threshold and the latch re-arms without re-firing.
    client->setConnectionParams({0x55, 0x66, 0x77, 0x88}, key(0x3C), dish::proto::kProtocolVersion);
    CHECK_FALSE(dish::reducer::counterNeedsRepush(client->sendCounter()));
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 1);

    SatelliteClientTestAccess::seedSendCounter(*client, dish::reducer::kCounterRepushThreshold);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 2);

    conn.markDisconnected();
    ::close(fd);
}

TEST_CASE("alive tick tolerates an absent rekey callback past the threshold", "[rekey]") {
    std::uint16_t port = 0;
    const int fd = bindLoopback(port);
    REQUIRE(fd >= 0);

    auto client = std::make_shared<SatelliteClient>();
    REQUIRE(client->openSocket("127.0.0.1", port));
    client->setConnectionParams({0x11, 0x22, 0x33, 0x44}, key(0xA5), dish::proto::kProtocolVersion);

    WifiConnection conn(QStringLiteral("mid:m2"), server());
    conn.markConnecting();
    conn.markConnected(client, QStringLiteral("conn_2"), /*epoch=*/0,
                       /*mouseControlGranted=*/false, {}, {}, {}, {});
    SatelliteClientTestAccess::seedSendCounter(*client, dish::reducer::kCounterRepushThreshold);
    WifiConnectionTestAccess::tick(conn); // must not crash
    conn.markDisconnected();
    ::close(fd);
}
