// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "LANDiscovery.h"

#include "Network/ScopedSocket.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <chrono>
#include <cstring>

namespace dish::net {

namespace {

// The beacon port is shared: several processes on this machine may be listening
// for the same broadcasts, so the bind must not claim it exclusively.
//
// The short receive timeout is what lets the loop notice its own deadline rather
// than blocking past it.
bool bindBeaconPort(int sock, int port) {
    const int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { return false; }

    timeval rtv{};
    rtv.tv_sec = 0;
    rtv.tv_usec = 300'000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    return true;
}

// The sender's address as text, empty when it cannot be read. Taken from the
// datagram rather than from the beacon body: a satellite behind NAT, or one that
// got its own address wrong, is still reachable at the address its packet came
// from.
QString senderAddress(const sockaddr_in& from) {
    char ipStr[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &from.sin_addr, ipStr, INET_ADDRSTRLEN) == nullptr) { return {}; }
    return QString::fromLatin1(ipStr);
}

} // namespace

// Listens; it never asks. A satellite broadcasts on its own cadence, so the whole
// timeout is waited out: unlike the mDNS scan there is no query to answer and
// nothing says more are not coming.
QList<models::DiscoveredServer> LANDiscovery::discover(int port, int timeoutMs) {
    using namespace std::chrono;

    const ScopedSocket sock;
    if (!sock.valid() || !bindBeaconPort(sock.get(), port)) { return {}; }

    const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    QSet<QString> seen;
    QList<models::DiscoveredServer> result;
    std::uint8_t buf[1024];

    while (steady_clock::now() < deadline) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        const ssize_t n =
            ::recvfrom(sock.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) { continue; }
        const QString ip = senderAddress(from);
        if (ip.isEmpty() || seen.contains(ip)) { continue; }
        const auto json =
            QString::fromUtf8(reinterpret_cast<const char*>(buf), static_cast<int>(n));
        // Marked seen only once a beacon parses: any other datagram on this port
        // would otherwise suppress the satellite for the rest of the scan.
        if (auto server = parseBeacon(json, ip)) {
            seen.insert(ip);
            result.append(*server);
        }
    }
    return result;
}

std::optional<models::DiscoveredServer> LANDiscovery::parseBeacon(const QString& json,
                                                                  const QString& observedIp) {
    // Structured parse first, then check the decoded `service` field — never a
    // substring probe of the raw text (JSON-hygiene rule shared org-wide).
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return std::nullopt; }
    const auto obj = doc.object();
    if (obj.value(QLatin1String("service")).toString() != QLatin1String("satellite")) {
        return std::nullopt;
    }
    auto server = models::DiscoveredServer::fromJson(obj);
    server.ip = observedIp;
    if (server.name.isEmpty()) { return std::nullopt; }
    return server;
}

} // namespace dish::net
