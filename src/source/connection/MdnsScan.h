// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One multicast-DNS query and the answers to it. Both discoverers speak the
// protocol directly rather than through a platform service - there is no Avahi
// dependency on Linux, and Windows offers only a COM-bound DNS-SD API - and this
// is the part of that they share: the socket, the query, and the receive window.
//
// What they do NOT share is the query bytes, the record parse, and what counts
// as the same responder twice. Those stay with each discoverer, which is why
// this takes a callback rather than a record type.
//
// No socket headers in this one on purpose: the discoverers include it, and
// nothing that calls a scan should need to see a sockaddr or windows.h. The
// dish-windows and dish-linux copies of this header are identical; only the .cpp
// differs, Winsock in one and POSIX in the other.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dish::net {

// The link-local mDNS group and port. Both discoverers query the same one; they
// differ only in the service name inside the query.
inline constexpr const char* kMdnsGroup = "224.0.0.251";
inline constexpr std::uint16_t kMdnsPort = 5353;

// How long the scan keeps listening after something answered. A responder that
// is there answers within a few hundred milliseconds, so once one has, waiting
// out the caller's whole timeout finds nothing and only delays the UI.
inline constexpr int kMdnsGraceMs = 600;

// Send `query` to the mDNS group from an ephemeral port, then hand every
// datagram that arrives to `onDatagram` until the window closes.
//
// Return TRUE from `onDatagram` for a datagram that yielded something new: that
// is what restarts the grace window. Returning true for a duplicate would hold
// a scan open for as long as one chatty responder keeps repeating itself.
//
// Blocking; call from a background thread. Does nothing if the socket cannot be
// opened or bound, which is indistinguishable to the caller from no answers,
// and is the right answer either way: there is no host it can reach.
void mdnsScan(const std::vector<std::uint8_t>& query, int timeoutMs,
              const std::function<bool(const std::uint8_t*, std::size_t)>& onDatagram);

} // namespace dish::net
