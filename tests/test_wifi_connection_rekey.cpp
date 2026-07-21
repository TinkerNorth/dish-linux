// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// The proactive re-key wiring (contract §Crypto: clients SHOULD re-PUT once
// the send counter crosses 0xF0000000): the alive tick must fire the manager's
// rekey hook when reducer::counterNeedsRepush trips, exactly once per
// approach, and re-arm only after the re-key lands (counter back at 1).
// The tick is driven directly through the test seam — no timer waits.

#include "Network/Reconcile.h"
#include "Network/WifiConnection.h"
#include "satellite_client_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

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

// QTimer (started by markConnected) needs an application object; Catch2's
// main doesn't make one.
void ensureApp() {
    if (QCoreApplication::instance() == nullptr) {
        static int argc = 1;
        static char name[] = "DishTests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

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

} // namespace

TEST_CASE("alive tick fires the rekey hook once per threshold approach and re-arms after landing",
          "[rekey]") {
    ensureApp();
    std::uint16_t port = 0;
    const int fd = bindLoopback(port);
    REQUIRE(fd >= 0);

    auto client = std::make_shared<SatelliteClient>();
    REQUIRE(client->openSocket("127.0.0.1", port));
    client->setConnectionParams({0x11, 0x22, 0x33, 0x44}, key(0xA5));

    dish::models::DiscoveredServer server;
    server.name = QStringLiteral("sat");
    server.ip = QStringLiteral("127.0.0.1");
    WifiConnection conn(QStringLiteral("test-id"), server);

    int rekeyCalls = 0;
    WifiConnection::SessionHooks hooks;
    hooks.rekey = [&rekeyCalls] { rekeyCalls++; };
    conn.markConnecting();
    conn.markConnected(client, QStringLiteral("conn_1"), /*epoch=*/0, hooks);

    // Below the threshold: no fire.
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 0);

    // Crossing 0xF0000000 fires the hook exactly once, not once per tick.
    SatelliteClientTestAccess::seedSendCounter(*client, dish::reducer::kCounterRepushThreshold);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 1);
    WifiConnectionTestAccess::tick(conn);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 1);

    // The re-key lands (fresh token/key, as runRekey installs): the counter
    // restarts at 1 and the latch re-arms without an immediate re-fire.
    client->setConnectionParams({0x55, 0x66, 0x77, 0x88}, key(0x3C));
    CHECK(client->sendCounter() == 1);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 1);

    // The next approach to exhaustion fires again.
    SatelliteClientTestAccess::seedSendCounter(*client, dish::reducer::kCounterRepushThreshold);
    WifiConnectionTestAccess::tick(conn);
    CHECK(rekeyCalls == 2);

    conn.markDisconnected();
    ::close(fd);
}

TEST_CASE("alive tick tolerates an absent rekey hook past the threshold", "[rekey]") {
    ensureApp();
    std::uint16_t port = 0;
    const int fd = bindLoopback(port);
    REQUIRE(fd >= 0);

    auto client = std::make_shared<SatelliteClient>();
    REQUIRE(client->openSocket("127.0.0.1", port));
    client->setConnectionParams({0x11, 0x22, 0x33, 0x44}, key(0xA5));

    WifiConnection conn(QStringLiteral("test-id-2"), dish::models::DiscoveredServer{});
    conn.markConnecting();
    conn.markConnected(client, QStringLiteral("conn_2"), /*epoch=*/0, {});
    SatelliteClientTestAccess::seedSendCounter(*client, dish::reducer::kCounterRepushThreshold);
    WifiConnectionTestAccess::tick(conn); // must not crash
    conn.markDisconnected();
    ::close(fd);
}
