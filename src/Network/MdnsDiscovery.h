// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QList>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dish::net {

// Discovers Satellite servers advertised over mDNS / Bonjour as the
// `_satellite._udp.local.` service type — the modern discovery path that
// reaches subnets where the legacy UDP broadcast beacon (LANDiscovery) is
// dropped. Pairs with satellite/src/net/mdns_responder.cpp.
//
// Linux normally has Avahi, but linking it would add a hard runtime
// dependency, so this is a one-shot raw multicast-DNS client: it sends a
// single PTR query (with the unicast-response bit set) to 224.0.0.251:5353
// and parses the PTR + SRV + TXT + A answer records the responder returns.
class MdnsDiscovery {
  public:
    static constexpr int kDefaultTimeoutMs = 4000;

    // Blocking. Call from a background thread. Returns every unique satellite
    // that answered within `timeoutMs`. Each recv has a short timeout so a
    // quiet network doesn't hang the caller past the deadline; once at least
    // one satellite has answered the wait is capped at a short grace window.
    static QList<models::DiscoveredServer> discover(int timeoutMs = kDefaultTimeoutMs);
};

// Merge the legacy-broadcast and mDNS discovery results, tagging each server's
// `source`. A server heard on both paths becomes DiscoverySource::Both;
// otherwise it carries the path that surfaced it. Result is name-sorted and
// de-duplicated by `ip:udpPort`. Pure — exercised by tests/test_mdns_discovery.
QList<models::DiscoveredServer> mergeDiscovered(const QList<models::DiscoveredServer>& broadcast,
                                                const QList<models::DiscoveredServer>& mdns);

// Pure mDNS wire helpers. Exposed only so tests/test_mdns_discovery.cpp can
// exercise the compression-pointer + bounds handling directly; production code
// reaches them through MdnsDiscovery::discover().
namespace detail {

// Read a DNS name at `off`, following 0xC0 compression pointers. Returns the
// bytes consumed at `off` (a pointer counts as 2), or 0 on malformed input.
std::size_t skipName(const std::uint8_t* p, std::size_t len, std::size_t off);

// Decode a DNS name into `out` (dot-separated, no trailing dot). Returns false
// on malformed input.
bool readName(const std::uint8_t* p, std::size_t len, std::size_t off, std::string& out);

// Parse one mDNS response packet into a DiscoveredServer (source = Mdns), or
// nullopt when it doesn't carry a usable satellite record set.
std::optional<models::DiscoveredServer> parseResponse(const std::uint8_t* p, std::size_t len);

} // namespace detail

} // namespace dish::net
