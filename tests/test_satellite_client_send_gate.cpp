// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The send path takes no lock: senders on several threads share the session through a seqlock
// on the material and a gate around the counter draw and sendto. Asserted on the wire over
// loopback UDP: counter order is wire order, a re-key never pairs a key with the other key's
// counters, and a receiver that never reads never blocks a sender. Linux caps a socket's receive
// buffer (net.core.rmem_max), so a reader drains while the senders send, and a datagram the
// receiver had no room for is a gap, never a reordering.

#include "Network/SatelliteClient.h"
#include "core/model/Protocol.h"
#include "core/wire/SessionCrypto.h"

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using dish::net::SatelliteClient;

namespace {

using Key = std::array<std::uint8_t, dish::wire::kCryptoKeySize>;
using Token = std::array<std::uint8_t, 4>;

constexpr Token kToken{0x11, 0x22, 0x33, 0x44};
constexpr std::uint32_t kTokenBe = 0x11223344;
constexpr std::size_t kHeader = 8;

Key keyOf(std::uint8_t fill) {
    Key k{};
    k.fill(fill);
    return k;
}

struct Receiver {
    int fd = -1;
    std::uint16_t port = 0;

    Receiver() {
        fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);
        timeval timeout{};
        timeout.tv_usec = 200'000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const int bigBuffer = 8 * 1024 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bigBuffer, sizeof(bigBuffer));
    }
    ~Receiver() { ::close(fd); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&) = delete;
    Receiver& operator=(Receiver&&) = delete;

    std::optional<std::vector<std::uint8_t>> next() const {
        std::array<std::uint8_t, dish::proto::kUdpDatagramMaxBytes> buf{};
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) { return std::nullopt; }
        return std::vector<std::uint8_t>(buf.begin(), buf.begin() + n);
    }
};

std::uint32_t counterOf(const std::vector<std::uint8_t>& pkt) {
    return (static_cast<std::uint32_t>(pkt[4]) << 24) | (static_cast<std::uint32_t>(pkt[5]) << 16) |
           (static_cast<std::uint32_t>(pkt[6]) << 8) | static_cast<std::uint32_t>(pkt[7]);
}

bool opensWith(const Key& key, const std::vector<std::uint8_t>& pkt) {
    std::array<std::uint8_t, dish::proto::kUdpDatagramMaxBytes> plain{};
    unsigned long long plainLen = 0;
    return pkt.size() > kHeader &&
           dish::wire::decryptPacket(key.data(), dish::wire::kDirClientToServer, counterOf(pkt),
                                     kTokenBe, pkt.data() + kHeader, pkt.size() - kHeader,
                                     plain.data(), &plainLen);
}

// Collects every datagram the receiver gets while the senders run, and the stragglers after.
class Drain {
  public:
    explicit Drain(const Receiver& receiver)
        : reader_([this, &receiver] {
              while (!stop_.load(std::memory_order_acquire)) {
                  if (auto pkt = receiver.next()) { packets_.push_back(std::move(*pkt)); }
              }
              while (auto pkt = receiver.next()) { packets_.push_back(std::move(*pkt)); }
          }) {}
    ~Drain() { finish(); }
    Drain(const Drain&) = delete;
    Drain& operator=(const Drain&) = delete;
    Drain(Drain&&) = delete;
    Drain& operator=(Drain&&) = delete;

    const std::vector<std::vector<std::uint8_t>>& finish() {
        stop_.store(true, std::memory_order_release);
        if (reader_.joinable()) { reader_.join(); }
        return packets_;
    }

  private:
    std::atomic<bool> stop_{false};
    std::vector<std::vector<std::uint8_t>> packets_;
    std::thread reader_;
};

void sendFrom(SatelliteClient& client, int threads, int reportsEach) {
    std::vector<std::thread> senders;
    senders.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        senders.emplace_back([&client, t, reportsEach] {
            for (int i = 0; i < reportsEach; ++i) {
                client.sendReport(0, static_cast<std::uint16_t>(t), 0, 0,
                                  static_cast<std::int16_t>(i), 0, 0, 0);
            }
        });
    }
    for (auto& s : senders) { s.join(); }
}

} // namespace

TEST_CASE("senders on several threads put their counters on the wire in order",
          "[satellite-client][send-gate]") {
    const Receiver receiver;
    SatelliteClient client;
    REQUIRE(client.openSocket("127.0.0.1", receiver.port));
    client.setConnectionParams(kToken, keyOf(0xA5), dish::proto::kProtocolVersion);

    constexpr int kThreads = 4;
    constexpr int kEach = 500;
    Drain drain(receiver);
    sendFrom(client, kThreads, kEach);

    std::vector<std::uint32_t> counters;
    for (const auto& pkt : drain.finish()) {
        REQUIRE(opensWith(keyOf(0xA5), pkt));
        counters.push_back(counterOf(pkt));
    }
    CHECK_FALSE(counters.empty());
    CHECK(client.sendCounter() == static_cast<std::uint32_t>(kThreads * kEach) + 1U);
    CHECK(std::adjacent_find(counters.begin(), counters.end(),
                             [](std::uint32_t a, std::uint32_t b) { return b <= a; }) ==
          counters.end());
    client.closeSocket();
}

TEST_CASE("a re-key during sends never pairs one key with the other key's counters",
          "[satellite-client][send-gate]") {
    const Receiver receiver;
    SatelliteClient client;
    REQUIRE(client.openSocket("127.0.0.1", receiver.port));
    const Key oldKey = keyOf(0xA5);
    const Key newKey = keyOf(0x5A);
    client.setConnectionParams(kToken, oldKey, dish::proto::kProtocolVersion);

    Drain drain(receiver);
    std::atomic<bool> stop{false};
    std::vector<std::thread> senders;
    for (int t = 0; t < 3; ++t) {
        senders.emplace_back([&client, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                client.sendReport(0, 1, 0, 0, 0, 0, 0, 0);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client.setConnectionParams(kToken, newKey, dish::proto::kProtocolVersion);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true);
    for (auto& s : senders) { s.join(); }

    int switches = 0;
    int oldSeen = 0;
    int newSeen = 0;
    bool onNew = false;
    std::uint32_t last = 0;
    bool increasingPerKey = true;
    bool exactlyOneKey = true;
    for (const auto& pkt : drain.finish()) {
        const bool byOld = opensWith(oldKey, pkt);
        const bool byNew = opensWith(newKey, pkt);
        if (byOld == byNew) {
            exactlyOneKey = false;
            continue;
        }
        if (byNew && !onNew) {
            onNew = true;
            ++switches;
            last = 0;
        }
        if (byOld && onNew) { exactlyOneKey = false; }
        const std::uint32_t counter = counterOf(pkt);
        if (counter <= last) { increasingPerKey = false; }
        last = counter;
        (byOld ? oldSeen : newSeen) += 1;
    }
    CHECK(exactlyOneKey);
    CHECK(increasingPerKey);
    CHECK(switches == 1);
    CHECK(oldSeen > 0);
    CHECK(newSeen > 0);
    client.closeSocket();
}

TEST_CASE("a send buffer the receiver never drains drops datagrams instead of blocking",
          "[satellite-client][send-gate]") {
    const Receiver paused;
    SatelliteClient client;
    REQUIRE(client.openSocket("127.0.0.1", paused.port));
    client.setConnectionParams(kToken, keyOf(0xA5), dish::proto::kProtocolVersion);

    const std::vector<std::uint8_t> opus(dish::proto::kAudioWireMaxOpusBytes, 0xF8);
    constexpr int kSends = 20'000;
    std::vector<double> callUs;
    callUs.reserve(kSends);
    for (int i = 0; i < kSends; ++i) {
        const auto started = std::chrono::steady_clock::now();
        client.sendMicAudio(0, static_cast<std::uint16_t>(i), opus.data(), opus.size());
        callUs.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
                .count());
    }
    std::sort(callUs.begin(), callUs.end());
    CHECK(callUs[callUs.size() / 2] < 1000.0);
    CHECK(callUs[callUs.size() * 99 / 100] < 1000.0);
    client.closeSocket();
}
