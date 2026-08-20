// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/connection/LANDiscovery.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

using dish::net::LANDiscovery;

namespace {

// Only the number is reserved: discover() does its own bind, so the probe socket
// has to be released first.
std::uint16_t freeUdpPort() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

} // namespace

TEST_CASE("parseBeacon accepts a valid satellite beacon", "[discovery]") {
    const QString payload = QStringLiteral(
        R"({"service":"satellite","name":"office","udpPort":9876,"pairPort":9878,"httpPort":9877})");
    const auto s = LANDiscovery::parseBeacon(payload, "10.0.0.1");
    REQUIRE(s.has_value());
    REQUIRE(s->name == "office");
    REQUIRE(s->ip == "10.0.0.1");
    REQUIRE(s->udpPort == 9876);
    REQUIRE(s->pairPort == 9878);
    REQUIRE(s->httpPort == 9877);
}

TEST_CASE("parseBeacon rejects payloads from other services", "[discovery]") {
    const QString payload = QStringLiteral(R"({"service":"chromecast","name":"foo"})");
    REQUIRE_FALSE(LANDiscovery::parseBeacon(payload, "10.0.0.1").has_value());
}

TEST_CASE("parseBeacon rejects malformed JSON", "[discovery]") {
    REQUIRE_FALSE(LANDiscovery::parseBeacon("not json", "10.0.0.1").has_value());
    REQUIRE_FALSE(
        LANDiscovery::parseBeacon(QStringLiteral(R"({"service":"satellite",)"), "10.0.0.1")
            .has_value());
}

TEST_CASE("parseBeacon rejects beacons with an empty name", "[discovery]") {
    const QString payload = QStringLiteral(R"({"service":"satellite","name":""})");
    REQUIRE_FALSE(LANDiscovery::parseBeacon(payload, "10.0.0.1").has_value());
}

TEST_CASE("parseBeacon overrides any beacon-supplied ip with the observed source", "[discovery]") {
    const QString payload =
        QStringLiteral(R"({"service":"satellite","name":"office","ip":"1.1.1.1"})");
    const auto s = LANDiscovery::parseBeacon(payload, "10.0.0.7");
    REQUIRE(s.has_value());
    REQUIRE(s->ip == "10.0.0.7");
}

TEST_CASE("a stray datagram does not suppress a later beacon from the same host", "[discovery]") {
    // Regression: the source address used to be marked seen before the parse, so
    // one truncated or unrelated datagram on the discovery port blacklisted that
    // satellite for the whole scan window.
    const std::uint16_t port = freeUdpPort();
    const QByteArray junk = QByteArrayLiteral(R"({"service":"satel)");
    const QByteArray beacon =
        QByteArrayLiteral(R"({"service":"satellite","name":"office","udpPort":9876})");

    std::atomic<bool> stop{false};
    std::thread sender([&] {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) { return; }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        // Repeated because the receiver's bind races this thread; junk always
        // leads, and the dup guard collapses the repeats into one row.
        while (!stop.load(std::memory_order_relaxed)) {
            ::sendto(fd, junk.constData(), static_cast<std::size_t>(junk.size()), 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to));
            ::sendto(fd, beacon.constData(), static_cast<std::size_t>(beacon.size()), 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to));
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        ::close(fd);
    });

    const auto found = LANDiscovery::discover(port, 600);
    stop.store(true, std::memory_order_relaxed);
    sender.join();

    REQUIRE(found.size() == 1); // still deduped: one row, not one per repeat
    CHECK(found.front().name == "office");
    CHECK(found.front().ip == "127.0.0.1");
    CHECK(found.front().udpPort == 9876);
}
