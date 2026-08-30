// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One-shot mDNS discovery of Moonlight hosts advertised under
// `_nvstream._tcp.local.`, the sibling of net::MdnsDiscovery's satellite
// query. Reuses that module's DNS wire helpers (net::detail::skipName /
// readName) and adds an SRV-target -> A record resolve, since GameStream hosts
// advertise their address indirectly. Blocking: call from a background thread.
// Manual host entry (source/moonlight/MoonlightSession) is the fallback when a
// network drops the multicast query.

#pragma once

#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace dish::source::moon {

struct DiscoveredMoonlightHost {
    QString name;    // the service instance label
    QString address; // resolved IPv4
    int httpPort = 47989;

    bool isValid() const { return !address.isEmpty(); }
    bool operator==(const DiscoveredMoonlightHost& o) const {
        return name == o.name && address == o.address && httpPort == o.httpPort;
    }
};

class MoonlightDiscovery {
  public:
    static constexpr int kDefaultTimeoutMs = 3000;

    // Sends one PTR query for `_nvstream._tcp.local.` and collects the hosts
    // that answer within the window.
    static QList<DiscoveredMoonlightHost> discover(int timeoutMs = kDefaultTimeoutMs);
};

namespace detail {

// Parses one mDNS response packet into a host, following the SRV target to its
// A record. nullopt when the packet carries no usable _nvstream record set.
// Exposed for unit tests, which feed it hand-built packets without a socket.
std::optional<DiscoveredMoonlightHost> parseMoonlightResponse(const std::uint8_t* p,
                                                              std::size_t len);

} // namespace detail

} // namespace dish::source::moon
