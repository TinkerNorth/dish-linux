// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QList>

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
    // quiet network doesn't hang the caller past the deadline.
    static QList<models::DiscoveredServer> discover(int timeoutMs = kDefaultTimeoutMs);
};

} // namespace dish::net
