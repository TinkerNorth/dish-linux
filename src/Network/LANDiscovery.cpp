// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "LANDiscovery.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace dish::net {

QList<models::DiscoveredServer> LANDiscovery::discover(int port, int timeoutMs) {
    using namespace std::chrono;

    const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { return {}; }

    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return {};
    }

    timeval rtv{};
    rtv.tv_sec = 0;
    rtv.tv_usec = 300'000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    QSet<QString> seen;
    QList<models::DiscoveredServer> result;
    std::uint8_t buf[1024];

    while (steady_clock::now() < deadline) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        const ssize_t n =
            ::recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) { continue; }
        const auto json =
            QString::fromUtf8(reinterpret_cast<const char*>(buf), static_cast<int>(n));
        char ipStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &from.sin_addr, ipStr, INET_ADDRSTRLEN);
        const QString ip = QString::fromLatin1(ipStr);
        if (seen.contains(ip)) { continue; }
        seen.insert(ip);
        if (auto server = parseBeacon(json, ip)) { result.append(*server); }
    }

    ::close(sock);
    return result;
}

std::optional<models::DiscoveredServer> LANDiscovery::parseBeacon(const QString& json,
                                                                  const QString& observedIp) {
    if (!json.contains(QStringLiteral("\"service\":\"satellite\""))) { return std::nullopt; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return std::nullopt; }
    auto server = models::DiscoveredServer::fromJson(doc.object());
    server.ip = observedIp;
    if (server.name.isEmpty()) { return std::nullopt; }
    return server;
}

} // namespace dish::net
