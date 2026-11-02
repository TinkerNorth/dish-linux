// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A session's client is closed on the reaper's thread, never on the thread that lets it go: a
// close joins the client's heartbeat and receive threads. A Faltering session that reconnects
// lets its client go first, so two never stream at once.

#include "Network/ClientReaper.h"
#include "Network/SatelliteClient.h"
#include "Network/WifiConnection.h"
#include "satellite_client_test_access.h"
#include "wifi_connection_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using dish::net::ClientReaper;
using dish::net::SatelliteClient;
using dish::net::SatelliteClientTestAccess;
using dish::net::SessionState;
using dish::net::WifiConnection;
using dish::net::WifiConnectionTestAccess;
using namespace std::chrono_literals;

namespace {

// Loose enough for a loaded runner under a sanitizer, and far under a close's joins.
constexpr auto kAtOnce = 20ms;

// A loopback port that takes the clients' datagrams and never answers.
struct Sink {
    int fd = -1;
    std::uint16_t port = 0;

    Sink() {
        fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);
    }
    ~Sink() { ::close(fd); }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;
};

std::shared_ptr<SatelliteClient> openClient(const Sink& sink) {
    auto client = std::make_shared<SatelliteClient>();
    REQUIRE(client->openSocket("127.0.0.1", sink.port));
    std::array<std::uint8_t, 32> key{};
    key.fill(0xA5);
    client->setConnectionParams({0x11, 0x22, 0x33, 0x44}, key, dish::proto::kProtocolVersion);
    return client;
}

// A client with both of its threads running, so a close has two joins to wait out.
std::shared_ptr<SatelliteClient> runningClient(const Sink& sink) {
    auto client = openClient(sink);
    client->startReceiveLoop();
    client->startHeartbeat();
    return client;
}

bool waitFor(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

dish::models::DiscoveredServer server() {
    dish::models::DiscoveredServer s;
    s.machineId = QStringLiteral("m1");
    s.ip = QStringLiteral("127.0.0.1");
    s.name = QStringLiteral("Sat");
    return s;
}

void connect(WifiConnection& conn, const std::shared_ptr<SatelliteClient>& client,
             const QString& connectionId) {
    conn.markConnecting();
    conn.markConnected(client, connectionId, /*epoch=*/0, /*mouseControlGranted=*/false, {}, {}, {},
                       {});
}

} // namespace

TEST_CASE("a retired client is closed on the reaper's thread while the caller goes on",
          "[client-reaper]") {
    const Sink sink;
    auto client = runningClient(sink);
    ClientReaper reaper;

    const auto started = std::chrono::steady_clock::now();
    reaper.retire(client);
    CHECK(std::chrono::steady_clock::now() - started < kAtOnce);

    CHECK(waitFor([&client] { return !client->isOpen(); }));
}

TEST_CASE("drain returns once every client retired before it is closed", "[client-reaper]") {
    const Sink sink;
    std::vector<std::shared_ptr<SatelliteClient>> clients;
    for (int i = 0; i < 3; ++i) { clients.push_back(runningClient(sink)); }
    ClientReaper reaper;
    for (const auto& client : clients) { reaper.retire(client); }

    reaper.drain();

    for (const auto& client : clients) { CHECK_FALSE(client->isOpen()); }
}

TEST_CASE("a reaper that goes away closes what is still queued", "[client-reaper]") {
    const Sink sink;
    auto first = runningClient(sink);
    auto second = runningClient(sink);
    {
        ClientReaper reaper;
        reaper.retire(first);
        reaper.retire(second);
    }
    CHECK_FALSE(first->isOpen());
    CHECK_FALSE(second->isOpen());
}

TEST_CASE("a Faltering session's reconnect lets its client go before the new one arrives",
          "[client-reaper][wifi]") {
    const Sink sink;
    auto reaper = std::make_shared<ClientReaper>();
    WifiConnection conn(WifiConnection::idFor(server()), server());
    conn.setClientReaper(reaper);
    auto old = openClient(sink);
    connect(conn, old, QStringLiteral("conn_1"));
    SatelliteClientTestAccess::seedMissedAcks(*old, SatelliteClient::kHeartbeatMissNotResponding);
    WifiConnectionTestAccess::tick(conn);
    REQUIRE(conn.state() == SessionState::Faltering);

    const auto started = std::chrono::steady_clock::now();
    conn.markConnecting();
    const auto took = std::chrono::steady_clock::now() - started;

    CHECK(conn.state() == SessionState::Linking);
    CHECK(conn.client() == nullptr);
    // The close, a heartbeat chunk and a receive wait, happens on the reaper's thread.
    CHECK(took < kAtOnce);
    CHECK(waitFor([&old] { return !old->isOpen(); }));

    auto fresh = openClient(sink);
    conn.markConnected(fresh, QStringLiteral("conn_2"), /*epoch=*/0,
                       /*mouseControlGranted=*/false, {}, {}, {}, {});
    CHECK(conn.client() == fresh);
    CHECK_FALSE(old->isOpen());

    conn.markDisconnected();
    reaper->drain();
    CHECK_FALSE(fresh->isOpen());
}

TEST_CASE("a session with no reaper closes the client it lets go of in place", "[client-reaper]") {
    const Sink sink;
    WifiConnection conn(WifiConnection::idFor(server()), server());
    auto client = openClient(sink);
    connect(conn, client, QStringLiteral("conn_1"));

    conn.markDisconnected();

    CHECK_FALSE(client->isOpen());
}
