// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "MdnsDiscovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace dish::net {

namespace {

// mDNS multicast group + port (RFC 6762).
constexpr const char* kMulticastGroup = "224.0.0.251";
constexpr std::uint16_t kMulticastPort = 5353;

constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeTxt = 16;
constexpr std::uint16_t kTypeSrv = 33;

// IN class with the QU (unicast-response) bit set — RFC 6762 §5.4. The
// responder then answers straight to our source port, so a one-shot client
// never has to join the multicast group to receive.
constexpr std::uint16_t kClassInQu = 0x8001;

// Straggler window after the first answer. The responder replies in well under
// a millisecond, so burning the full discovery window buys nothing.
constexpr int kGraceMs = 600;

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// The PTR query for `_satellite._udp.local.`: 12-byte header + the question
// (length-prefixed labels + type + class).
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

} // namespace

namespace detail {

std::size_t skipName(const std::uint8_t* p, std::size_t len, std::size_t off) {
    std::size_t consumed = 0;
    bool jumped = false;
    std::size_t guard = 0;
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
            const std::size_t target = (static_cast<std::size_t>(b & 0x3F) << 8) | p[off + 1];
            if (target >= off) { return 0; } // only backward jumps
            off = target;
            jumped = true;
            continue;
        }
        const std::size_t label = b + 1;
        if (off + label > len) { return 0; }
        if (!jumped) { consumed += label; }
        off += label;
    }
    return 0;
}

bool readName(const std::uint8_t* p, std::size_t len, std::size_t off, std::string& out) {
    out.clear();
    std::size_t guard = 0;
    while (off < len) {
        if (++guard > len) { return false; }
        const std::uint8_t b = p[off];
        if (b == 0) { return true; }
        if ((b & 0xC0) == 0xC0) {
            if (off + 1 >= len) { return false; }
            const std::size_t target = (static_cast<std::size_t>(b & 0x3F) << 8) | p[off + 1];
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

// The responder packs SRV + A + TXT into a single packet, so a per-packet parse
// is sufficient — no cross-packet record assembly needed.
std::optional<models::DiscoveredServer> parseResponse(const std::uint8_t* p, std::size_t len) {
    if (len < 12) { return std::nullopt; }
    const std::uint16_t qd = read16(p + 4);
    const std::uint16_t an = read16(p + 6);
    std::size_t pos = 12;

    // Skip the question section.
    for (std::uint16_t i = 0; i < qd; ++i) {
        const std::size_t consumed = skipName(p, len, pos);
        if (consumed == 0) { return std::nullopt; }
        pos += consumed + 4; // + type + class
        if (pos > len) { return std::nullopt; }
    }

    std::string ip;
    std::string instance;
    int srvPort = 0; // 0 = no SRV seen; the mapping layer applies precedence
    QHash<QString, QByteArray> txt;
    bool haveSrv = false;
    bool haveTxt = false;

    for (std::uint16_t i = 0; i < an; ++i) {
        const std::size_t nameLen = skipName(p, len, pos);
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
            if (::inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr) { ip = buf; }
        } else if (type == kTypeSrv && rdlen >= 7) {
            srvPort = read16(p + rdata + 4); // priority(2) weight(2) port(2)
            haveSrv = true;
        } else if (type == kTypeTxt) {
            std::size_t t = rdata;
            const std::size_t end = rdata + rdlen;
            while (t < end) {
                const std::uint8_t slen = p[t];
                if (t + 1 + slen > end) { break; }
                const std::string entry(reinterpret_cast<const char*>(p + t + 1), slen);
                const auto eq = entry.find('=');
                if (eq != std::string::npos) {
                    const QString key = QString::fromStdString(entry.substr(0, eq));
                    const QByteArray val = QByteArray::fromStdString(entry.substr(eq + 1));
                    txt.insert(key, val);
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
    return mdnsServiceToServer(QString::fromStdString(instance), QString::fromStdString(ip),
                               srvPort, txt);
}

} // namespace detail

std::optional<int> mdnsTxtInt(const QHash<QString, QByteArray>& txt, const QString& key) {
    const auto it = txt.constFind(key);
    if (it == txt.constEnd() || it->isNull()) { return std::nullopt; }
    const QString trimmed = QString::fromUtf8(*it).trimmed();
    if (trimmed.isEmpty()) { return std::nullopt; }
    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (!ok) { return std::nullopt; }
    return value;
}

std::optional<QString> mdnsTxtString(const QHash<QString, QByteArray>& txt, const QString& key) {
    const auto it = txt.constFind(key);
    if (it == txt.constEnd() || it->isNull()) { return std::nullopt; }
    // Non-const on purpose: const blocks the implicit move into the returned
    // optional and costs a QString copy on every hit.
    QString trimmed = QString::fromUtf8(*it).trimmed();
    if (trimmed.isEmpty()) { return std::nullopt; }
    return trimmed;
}

std::optional<models::DiscoveredServer> mdnsServiceToServer(const QString& serviceName,
                                                            const QString& hostAddress, int srvPort,
                                                            const QHash<QString, QByteArray>& txt) {
    if (hostAddress.isEmpty()) { return std::nullopt; } // null host → nothing to connect to

    models::DiscoveredServer s;
    s.name = serviceName.isEmpty() ? hostAddress : serviceName;
    s.ip = hostAddress;
    if (const auto txtUdp = mdnsTxtInt(txt, QStringLiteral("udp"))) {
        s.udpPort = *txtUdp;
    } else if (srvPort > 0) {
        s.udpPort = srvPort;
    } else {
        s.udpPort = kMdnsDefaultUdp;
    }
    s.pairPort = mdnsTxtInt(txt, QStringLiteral("pair")).value_or(kMdnsDefaultPair);
    s.httpPort = mdnsTxtInt(txt, QStringLiteral("http")).value_or(kMdnsDefaultHttp);
    s.machineId = mdnsTxtString(txt, QStringLiteral("mid")).value_or(QString());
    s.source = models::DiscoverySource::Mdns;
    return s;
}

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
    const auto hardDeadline = steady_clock::now() + milliseconds(timeoutMs);
    auto deadline = hardDeadline;
    std::uint8_t buf[2048];

    while (steady_clock::now() < deadline) {
        const ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) { continue; } // timeout / transient
        const auto server = detail::parseResponse(buf, static_cast<std::size_t>(n));
        if (!server) { continue; }
        const QString key = server->ip + QStringLiteral(":") + QString::number(server->udpPort);
        if (seen.contains(key)) { continue; }
        seen.insert(key);
        result.append(*server);
        // Cap the remaining wait so a launch-time scan returns promptly instead
        // of idling out the full `timeoutMs`.
        deadline = std::min(hardDeadline, steady_clock::now() + milliseconds(kGraceMs));
    }

    ::close(sock);
    return result;
}

} // namespace dish::net
