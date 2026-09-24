// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "MdnsScan.h"

#include "Network/ScopedSocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>

namespace dish::net {

namespace {

// An ephemeral local port, which is what makes the responders unicast their
// answers back here rather than to the group.
bool bindEphemeral(int sock) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    return ::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
}

// The short receive timeout is what lets the loop notice its own deadline rather
// than blocking past it; TTL 255 is what mDNS requires of a link-local query.
void applyScanOptions(int sock) {
    timeval rcvTimeout{};
    rcvTimeout.tv_sec = 0;
    rcvTimeout.tv_usec = 300'000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));
    const int ttl = 255;
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
}

void sendQuery(int sock, const std::vector<std::uint8_t>& query) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kMdnsPort);
    ::inet_pton(AF_INET, kMdnsGroup, &dest.sin_addr);
    ::sendto(sock, query.data(), query.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

} // namespace

void mdnsScan(const std::vector<std::uint8_t>& query, int timeoutMs,
              const std::function<bool(const std::uint8_t*, std::size_t)>& onDatagram) {
    using namespace std::chrono;

    const ScopedSocket sock;
    if (!sock.valid() || !bindEphemeral(sock.get())) { return; }
    applyScanOptions(sock.get());
    sendQuery(sock.get(), query);

    const auto hardDeadline = steady_clock::now() + milliseconds(timeoutMs);
    auto deadline = hardDeadline;
    std::uint8_t buf[2048];

    while (steady_clock::now() < deadline) {
        const ssize_t n = ::recvfrom(sock.get(), buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) { continue; } // timeout / transient
        if (!onDatagram(buf, static_cast<std::size_t>(n))) { continue; }
        // Never past the caller's own deadline: the grace window shortens the
        // wait, it does not extend it.
        deadline = std::min(hardDeadline, steady_clock::now() + milliseconds(kMdnsGraceMs));
    }
}

} // namespace dish::net
