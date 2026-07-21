// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// The send-counter never-wrap invariant (contract §Crypto): counters are
// monotonic per direction, never repeat a value under one session key, and a
// session that exhausts the 2^32 space goes SILENT instead of wrapping into
// ChaCha20-Poly1305 nonce reuse. Exercised over a real loopback UDP socket so
// the assertion is on the wire bytes, not on internal state alone.

#include "Network/Reconcile.h"
#include "Network/SatelliteClient.h"
#include "Network/SessionCrypto.h"
#include "satellite_client_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

using dish::net::SatelliteClient;
using dish::net::SatelliteClientTestAccess;

namespace {

// Bind an ephemeral loopback UDP socket the client under test sends into.
// Returns the fd (or -1) and fills `port`.
int bindLoopback(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { return -1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    timeval rtv{};
    rtv.tv_sec = 0;
    rtv.tv_usec = 200'000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    return fd;
}

std::optional<std::vector<std::uint8_t>> recvDatagram(int fd) {
    std::uint8_t buf[256];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) { return std::nullopt; }
    return std::vector<std::uint8_t>(buf, buf + n);
}

// Cleartext header: token(4) | counter(4 BE) | ciphertext+tag.
std::uint32_t counterOf(const std::vector<std::uint8_t>& pkt) {
    REQUIRE(pkt.size() >= 8);
    return (static_cast<std::uint32_t>(pkt[4]) << 24) | (static_cast<std::uint32_t>(pkt[5]) << 16) |
           (static_cast<std::uint32_t>(pkt[6]) << 8) | static_cast<std::uint32_t>(pkt[7]);
}

struct LoopbackClient {
    int fd = -1;
    std::uint16_t port = 0;
    SatelliteClient client;

    LoopbackClient() {
        fd = bindLoopback(port);
        REQUIRE(fd >= 0);
        REQUIRE(client.openSocket("127.0.0.1", port));
        client.setConnectionParams({0x11, 0x22, 0x33, 0x44}, key(0xA5));
    }
    ~LoopbackClient() {
        client.closeSocket();
        if (fd >= 0) { ::close(fd); }
    }
    static std::array<std::uint8_t, 32> key(std::uint8_t fill) {
        std::array<std::uint8_t, 32> k{};
        k.fill(fill);
        return k;
    }
    void sendOne() { client.sendBattery(0, 50, SatelliteClient::kBatteryStatusDischarging); }
};

} // namespace

TEST_CASE("send counter stamps monotonic wire values from 1 and the accessor tracks next-to-use",
          "[send_counter]") {
    LoopbackClient lb;
    lb.sendOne();
    lb.sendOne();
    const auto first = recvDatagram(lb.fd);
    const auto second = recvDatagram(lb.fd);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(counterOf(*first) == 1);
    CHECK(counterOf(*second) == 2);
    CHECK(lb.client.sendCounter() == 3);
}

TEST_CASE("send path goes silent at counter exhaustion instead of wrapping into nonce reuse",
          "[send_counter]") {
    LoopbackClient lb;
    SatelliteClientTestAccess::seedSendCounter(lb.client, 0xFFFFFFFFu);

    lb.sendOne();
    const auto last = recvDatagram(lb.fd);
    REQUIRE(last.has_value());
    CHECK(counterOf(*last) == 0xFFFFFFFFu); // the final counter value is still usable

    lb.sendOne();
    lb.sendOne();
    CHECK_FALSE(recvDatagram(lb.fd).has_value()); // silent: no wrapped-counter packets

    // The exhausted counter must keep reading re-PUT needed, never wrap back
    // under the threshold — the alive tick's guard depends on it.
    CHECK(lb.client.sendCounter() == 0xFFFFFFFFu);
    CHECK(dish::reducer::counterNeedsRepush(lb.client.sendCounter()));
}

TEST_CASE("a session never repeats a counter value under one key; re-key restarts at 1",
          "[send_counter]") {
    LoopbackClient lb;
    SatelliteClientTestAccess::seedSendCounter(lb.client, 0xFFFFFFFDu);

    // Drive across the exhaustion boundary: 3 valid values remain, then park.
    for (int i = 0; i < 6; ++i) { lb.sendOne(); }
    std::set<std::uint32_t> seen;
    std::uint32_t prev = 0;
    while (const auto pkt = recvDatagram(lb.fd)) {
        const std::uint32_t ctr = counterOf(*pkt);
        CHECK(ctr > prev); // strictly monotonic on the wire
        prev = ctr;
        CHECK(seen.insert(ctr).second); // no value ever repeats
    }
    CHECK(seen.size() == 3);

    // Re-key (fresh token + key, as the proactive re-PUT installs): counters
    // restart at 1 in a fresh nonce space and traffic flows again.
    lb.client.setConnectionParams({0x55, 0x66, 0x77, 0x88}, LoopbackClient::key(0x3C));
    CHECK(lb.client.sendCounter() == 1);
    lb.sendOne();
    const auto fresh = recvDatagram(lb.fd);
    REQUIRE(fresh.has_value());
    CHECK(counterOf(*fresh) == 1);
}

TEST_CASE("a live re-key never tears the (key, token, counter) draw", "[send_counter]") {
    // Hammer the send path from two threads while the owner thread re-keys
    // through many generations. Every packet on the wire must decrypt under
    // the key its token selects, and no (token, counter) pair may repeat —
    // torn material (old key + fresh counter, or half-swapped token/key)
    // fails one of the two.
    LoopbackClient lb;
    const int rcvbuf = 1 << 20; // best effort — drops are fine, mixups are not
    ::setsockopt(lb.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    constexpr std::uint8_t kGens = 40;
    const auto tokenFor = [](std::uint8_t gen) {
        return std::array<std::uint8_t, 4>{0xA0, 0x00, 0x00, gen};
    };
    const auto keyFor = [](std::uint8_t gen) {
        return LoopbackClient::key(static_cast<std::uint8_t>(gen ^ 0x5A));
    };
    lb.client.setConnectionParams(tokenFor(0), keyFor(0));

    std::atomic<bool> stop{false};
    const auto sender = [&lb, &stop] {
        for (int i = 0; i < 1500 && !stop.load(std::memory_order_relaxed); ++i) { lb.sendOne(); }
    };
    std::thread a(sender);
    std::thread b(sender);
    for (std::uint8_t gen = 1; gen <= kGens; ++gen) {
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        lb.client.setConnectionParams(tokenFor(gen), keyFor(gen));
    }
    a.join();
    b.join();
    stop.store(true, std::memory_order_relaxed);

    std::set<std::pair<std::uint8_t, std::uint32_t>> seen;
    int decrypted = 0;
    while (const auto pkt = recvDatagram(lb.fd)) {
        REQUIRE(pkt->size() >= 8 + 16);
        REQUIRE((*pkt)[0] == 0xA0);
        const std::uint8_t gen = (*pkt)[3];
        REQUIRE(gen <= kGens);
        const std::uint32_t ctr = counterOf(*pkt);
        const std::uint32_t tokenBe = (0xA0u << 24) | gen;
        std::vector<std::uint8_t> plain(pkt->size() - 8);
        unsigned long long plainLen = 0;
        REQUIRE(dish::wire::decryptPacket(keyFor(gen).data(), dish::wire::kDirClientToServer, ctr,
                                          tokenBe, pkt->data() + 8, pkt->size() - 8, plain.data(),
                                          &plainLen));
        REQUIRE(seen.insert({gen, ctr}).second); // nonce (dir|counter) unique per key
        decrypted++;
    }
    CHECK(decrypted > 0);
}
