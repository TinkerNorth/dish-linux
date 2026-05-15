// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "MdnsDiscovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <QSet>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace dish::net {

namespace {

// mDNS multicast group + port (RFC 6762).
constexpr const char* kMulticastGroup = "224.0.0.251";
constexpr std::uint16_t kMulticastPort = 5353;

// DNS record types we care about.
constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeTxt = 16;
constexpr std::uint16_t kTypeSrv = 33;

// IN class with the QU (unicast-response) bit set — RFC 6762 §5.4. Asking for
// a unicast reply means the responder answers straight to our source port, so
// a one-shot client doesn't need to join the multicast group to receive.
constexpr std::uint16_t kClassInQu = 0x8001;

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// Build the PTR query for `_satellite._udp.local.`. 12-byte header + the
// question (length-prefixed labels + type + class).
std::vector<std::uint8_t> buildQuery() {
    std::vector<std::uint8_t> q;
    const std::uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}; // qdcount = 1
    q.insert(q.end(), header, header + 12);
    for (const char* label : {"_satellite", "_udp", "local"}) {
        const auto len = static_cast<std::uint8_t>(std::strlen(label));
        q.push_back(len);
        q.insert(q.end(), label, label + len);
    }
    q.push_back(0); // root label
    q.push_back(static_cast<std::uint8_t>(kTypePtr >> 8));
    q.push_back(static_cast<std::uint8_t>(kTypePtr & 0xFF));
    q.push_back(static_cast<std::uint8_t>(kClassInQu >> 8));
    q.push_back(static_cast<std::uint8_t>(kClassInQu & 0xFF));
    return q;
}

// Read a DNS name at `off`, following 0xC0 compression pointers. Returns the
// bytes consumed *at the original offset* (a pointer counts as 2), or 0 on a
// malformed packet.
size_t skipName(const std::uint8_t* p, size_t len, size_t off) {
    size_t consumed = 0;
    bool jumped = false;
    size_t guard = 0;
    while (off < len) {
        if (++guard > len) { return 0; } // malformed: loop guard
        const std::uint8_t b = p[off];
        if (b == 0) {
            if (!jumped) { consumed += 1; }
            return consumed;
        }
        if ((b & 0xC0) == 0xC0) { // compression pointer
            if (off + 1 >= len) { return 0; }
            if (!jumped) { consumed += 2; }
            const size_t target = (static_cast<size_t>(b & 0x3F) << 8) | p[off + 1];
            if (target >= off) { return 0; } // only backward jumps
            off = target;
            jumped = true;
            continue;
        }
        const size_t label = b + 1;
        if (off + label > len) { return 0; }
        if (!jumped) { consumed += label; }
        off += label;
    }
    return 0;
}

// Read a DNS name into `out` (dot-separated, no trailing dot). Returns false
// on malformed input.
bool readName(const std::uint8_t* p, size_t len, size_t off, std::string& out) {
    out.clear();
    size_t guard = 0;
    while (off < len) {
        if (++guard > len) { return false; }
        const std::uint8_t b = p[off];
        if (b == 0) { return true; }
        if ((b & 0xC0) == 0xC0) {
            if (off + 1 >= len) { return false; }
            const size_t target = (static_cast<size_t>(b & 0x3F) << 8) | p[off + 1];
            if (target >= off) { return false; }
            off = target;
            continue;
        }
        if (off + 1 + b > len) { return false; }
        if (!out.empty()) { out += '.'; }
        out.append(reinterpret_cast<const char*>(p + off + 1), b);
        off += b + 1;
    }
    return false;
}

// Parse one mDNS response packet into a DiscoveredServer, if it carries the
// SRV + A + TXT records of a satellite. The responder packs all of those into
// a single packet, so a per-packet parse is sufficient.
std::optional<models::DiscoveredServer> parseResponse(const std::uint8_t* p, size_t len) {
    if (len < 12) { return std::nullopt; }
    const std::uint16_t qd = read16(p + 4);
    const std::uint16_t an = read16(p + 6);
    size_t pos = 12;

    // Skip the question section.
    for (std::uint16_t i = 0; i < qd; ++i) {
        const size_t consumed = skipName(p, len, pos);
        if (consumed == 0) { return std::nullopt; }
        pos += consumed + 4; // + type + class
        if (pos > len) { return std::nullopt; }
    }

    std::string ip;
    std::string instance;
    int udpPort = 9876;
    int pairPort = 9878;
    int httpPort = 9877;
    bool haveSrv = false;
    bool haveTxt = false;

    for (std::uint16_t i = 0; i < an; ++i) {
        const size_t nameLen = skipName(p, len, pos);
        if (nameLen == 0) { return std::nullopt; }
        pos += nameLen;
        if (pos + 10 > len) { return std::nullopt; }
        const std::uint16_t type = read16(p + pos);
        const std::uint16_t rdlen = read16(p + pos + 8);
        const size_t rdata = pos + 10;
        if (rdata + rdlen > len) { return std::nullopt; }

        if (type == kTypeA && rdlen == 4) {
            char buf[INET_ADDRSTRLEN] = {};
            in_addr a{};
            std::memcpy(&a, p + rdata, 4);
            if (::inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr) { ip = buf; }
        } else if (type == kTypeSrv && rdlen >= 7) {
            udpPort = read16(p + rdata + 4); // priority(2) weight(2) port(2)
            haveSrv = true;
        } else if (type == kTypeTxt) {
            size_t t = rdata;
            const size_t end = rdata + rdlen;
            while (t < end) {
                const std::uint8_t slen = p[t];
                if (t + 1 + slen > end) { break; }
                const std::string entry(reinterpret_cast<const char*>(p + t + 1), slen);
                const auto eq = entry.find('=');
                if (eq != std::string::npos) {
                    const std::string key = entry.substr(0, eq);
                    const int val = std::atoi(entry.c_str() + eq + 1);
                    if (key == "udp" && val > 0) { udpPort = val; }
                    if (key == "pair" && val > 0) { pairPort = val; }
                    if (key == "http" && val > 0) { httpPort = val; }
                }
                t += 1 + slen;
                haveTxt = true;
            }
        } else if (type == kTypePtr && instance.empty()) {
            std::string n;
            if (readName(p, len, rdata, n)) {
                // The instance label is the first component of the PTR target.
                instance = n.substr(0, n.find('.'));
            }
        }
        pos = rdata + rdlen;
    }

    if (ip.empty() || (!haveSrv && !haveTxt)) { return std::nullopt; }
    models::DiscoveredServer s;
    s.name = instance.empty() ? QString::fromStdString(ip) : QString::fromStdString(instance);
    s.ip = QString::fromStdString(ip);
    s.udpPort = udpPort;
    s.pairPort = pairPort;
    s.httpPort = httpPort;
    return s;
}

} // namespace

QList<models::DiscoveredServer> MdnsDiscovery::discover(int timeoutMs) {
    using namespace std::chrono;

    const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { return {}; }

    // Bind an ephemeral port; the QU bit makes responders unicast back here.
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

    QList<models::DiscoveredServer> result;
    QSet<QString> seen;
    const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    std::uint8_t buf[2048];

    while (steady_clock::now() < deadline) {
        const ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) { continue; } // timeout / transient
        const auto server = parseResponse(buf, static_cast<size_t>(n));
        if (!server) { continue; }
        const QString key = server->ip + QStringLiteral(":") + QString::number(server->udpPort);
        if (seen.contains(key)) { continue; }
        seen.insert(key);
        result.append(*server);
    }

    ::close(sock);
    return result;
}

} // namespace dish::net
