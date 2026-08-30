// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightDiscovery.h"

#include "source/connection/MdnsDiscovery.h" // net::detail::skipName / readName

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <QSet>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>

namespace dish::source::moon {
namespace {

constexpr const char* kMulticastGroup = "224.0.0.251";
constexpr std::uint16_t kMulticastPort = 5353;

constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeSrv = 33;
constexpr std::uint16_t kClassInQu = 0x8001;
constexpr int kGraceMs = 600;

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::vector<std::uint8_t> buildQuery() {
    std::vector<std::uint8_t> q;
    const std::uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    q.insert(q.end(), header, header + 12);
    for (const char* label : {"_nvstream", "_tcp", "local"}) {
        const auto len = static_cast<std::uint8_t>(std::strlen(label));
        q.push_back(len);
        q.insert(q.end(), label, label + len);
    }
    q.push_back(0);
    q.push_back(static_cast<std::uint8_t>(kTypePtr >> 8));
    q.push_back(static_cast<std::uint8_t>(kTypePtr & 0xFF));
    q.push_back(static_cast<std::uint8_t>(kClassInQu >> 8));
    q.push_back(static_cast<std::uint8_t>(kClassInQu & 0xFF));
    return q;
}

} // namespace

namespace detail {

std::optional<DiscoveredMoonlightHost> parseMoonlightResponse(const std::uint8_t* p,
                                                              std::size_t len) {
    if (len < 12) { return std::nullopt; }
    const std::uint16_t qd = read16(p + 4);
    const std::uint16_t an = read16(p + 6);
    std::size_t pos = 12;

    for (std::uint16_t i = 0; i < qd; ++i) {
        const std::size_t consumed = net::detail::skipName(p, len, pos);
        if (consumed == 0) { return std::nullopt; }
        pos += consumed + 4;
        if (pos > len) { return std::nullopt; }
    }

    std::string instance;
    std::string srvTarget;
    int srvPort = 0;
    // A GameStream reply packs the SRV (with the host's target name) and that
    // target's A record; resolve one against the other by name.
    std::unordered_map<std::string, std::string> aRecords;

    for (std::uint16_t i = 0; i < an; ++i) {
        std::string owner;
        net::detail::readName(p, len, pos, owner);
        const std::size_t nameLen = net::detail::skipName(p, len, pos);
        if (nameLen == 0) { return std::nullopt; }
        pos += nameLen;
        if (pos + 10 > len) { return std::nullopt; }
        const std::uint16_t type = read16(p + pos);
        const std::uint16_t rdlen = read16(p + pos + 8);
        const std::size_t rdata = pos + 10;
        if (rdata + rdlen > len) { return std::nullopt; }

        if (type == kTypeA && rdlen == 4) {
            char buf[INET_ADDRSTRLEN] = {};
            in_addr a{};
            std::memcpy(&a, p + rdata, 4);
            if (::inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr) {
                aRecords.emplace(owner, buf);
            }
        } else if (type == kTypeSrv && rdlen >= 7) {
            srvPort = read16(p + rdata + 4);
            std::string target;
            if (net::detail::readName(p, len, rdata + 6, target)) { srvTarget = target; }
        } else if (type == kTypePtr && instance.empty()) {
            std::string n;
            if (net::detail::readName(p, len, rdata, n)) { instance = n.substr(0, n.find('.')); }
        }
        pos = rdata + rdlen;
    }

    std::string address;
    if (!srvTarget.empty()) {
        const auto it = aRecords.find(srvTarget);
        if (it != aRecords.end()) { address = it->second; }
    }
    if (address.empty() && !aRecords.empty()) { address = aRecords.begin()->second; }
    if (address.empty()) { return std::nullopt; }

    DiscoveredMoonlightHost host;
    host.name =
        instance.empty() ? QString::fromStdString(address) : QString::fromStdString(instance);
    host.address = QString::fromStdString(address);
    // The SRV port advertises the HTTPS port; the plain HTTP port is the
    // GameStream default. Hosts do not advertise it, so keep the default.
    host.httpPort = 47989;
    (void)srvPort;
    return host;
}

} // namespace detail

QList<DiscoveredMoonlightHost> MoonlightDiscovery::discover(int timeoutMs) {
    using namespace std::chrono;

    const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { return {}; }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(sock);
        return {};
    }

    timeval rcvTimeout{};
    rcvTimeout.tv_sec = 0;
    rcvTimeout.tv_usec = 300'000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));
    int ttl = 255;
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kMulticastPort);
    ::inet_pton(AF_INET, kMulticastGroup, &dest.sin_addr);

    const auto query = buildQuery();
    ::sendto(sock, query.data(), query.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    QList<DiscoveredMoonlightHost> result;
    QSet<QString> seen;
    const auto hardDeadline = steady_clock::now() + milliseconds(timeoutMs);
    auto deadline = hardDeadline;
    std::uint8_t buf[2048];

    while (steady_clock::now() < deadline) {
        const ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) { continue; }
        const auto host = detail::parseMoonlightResponse(buf, static_cast<std::size_t>(n));
        if (!host) { continue; }
        if (seen.contains(host->address)) { continue; }
        seen.insert(host->address);
        result.append(*host);
        deadline = std::min(hardDeadline, steady_clock::now() + milliseconds(kGraceMs));
    }

    ::close(sock);
    return result;
}

} // namespace dish::source::moon
