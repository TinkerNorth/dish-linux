// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dish::net {

// Discovers satellites advertised over mDNS as `_satellite._udp.local.` — the
// path that reaches subnets where the UDP broadcast beacon (LANDiscovery) is
// dropped.
//
// Avahi is the usual answer here, but linking it would add a hard runtime
// dependency on a daemon many minimal installs omit. So this is a one-shot raw
// multicast-DNS client: one PTR query
// (unicast-response bit set) to 224.0.0.251:5353, then a parse of the PTR + SRV
// + TXT + A answer records. The wire-DNS layer is in detail::; the
// service→DiscoveredServer mapping below is split out so port precedence and
// TXT extraction are testable without a socket.
class MdnsDiscovery {
  public:
    static constexpr int kDefaultTimeoutMs = 4000;

    // Blocking; call from a background thread. Each recv has its own short
    // timeout so a quiet network can't hang the caller past `timeoutMs`, and
    // once one satellite has answered the wait is capped at a grace window.
    static QList<models::DiscoveredServer> discover(int timeoutMs = kDefaultTimeoutMs);
};

// ── mDNS service→DiscoveredServer mapping layer ─────────────────────────────
// Pure functions over a parsed (serviceName, hostAddress, SRV port, TXT map)
// tuple. Exposed for unit tests.

// Protocol-default ports when neither TXT nor SRV supplies one. The client API
// is HTTPS on a single port (9443) advertised under `pair` and `http`.
inline constexpr int kMdnsDefaultUdp = 9876;
inline constexpr int kMdnsDefaultPair = 9443;
inline constexpr int kMdnsDefaultHttp = 9443;

// nullopt when the key is missing, null/empty, or not fully numeric.
std::optional<int> mdnsTxtInt(const QHash<QString, QByteArray>& txt, const QString& key);

// TXT[key] trimmed, or nullopt when missing / whitespace-only.
std::optional<QString> mdnsTxtString(const QHash<QString, QByteArray>& txt, const QString& key);

// Assemble a DiscoveredServer from one resolved mDNS service. An empty
// `hostAddress` models a null host and yields nullopt (nothing to connect to);
// an empty service name falls back to the observed IP. Precedence: udpPort is
// TXT "udp" > SRV port (only when > 0) > 9876; pairPort / httpPort are TXT
// "pair"/"http" > 9443, never SRV.
std::optional<models::DiscoveredServer> mdnsServiceToServer(const QString& serviceName,
                                                            const QString& hostAddress, int srvPort,
                                                            const QHash<QString, QByteArray>& txt);

// Pure mDNS wire helpers. Exposed only so the tests can exercise the
// compression-pointer and bounds handling directly; production code reaches
// them through MdnsDiscovery::discover().
namespace detail {

// Bytes consumed at `off`, following 0xC0 compression pointers (a pointer
// counts as 2), or 0 on malformed input.
std::size_t skipName(const std::uint8_t* p, std::size_t len, std::size_t off);

// Dot-separated, no trailing dot. False on malformed input.
bool readName(const std::uint8_t* p, std::size_t len, std::size_t off, std::string& out);

// nullopt when the packet carries no usable satellite record set.
std::optional<models::DiscoveredServer> parseResponse(const std::uint8_t* p, std::size_t len);

} // namespace detail

} // namespace dish::net
