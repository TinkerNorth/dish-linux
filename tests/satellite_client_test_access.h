// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/SatelliteClient.h"

#include <cstdint>

namespace dish::net {

// The one definition of the test-only friend seam declared in SatelliteClient.h,
// shared by every test TU that needs it (ODR).
class SatelliteClientTestAccess {
  public:
    // Parks the send counter so nonce exhaustion is reachable without 2^64 sends.
    // `next` is the value sendEncrypted draws next.
    static void seedSendCounter(SatelliteClient& client, std::uint64_t next) {
        client.sendCounter_.reset(next);
    }

    // Drives the receive dispatch with a fully framed datagram, so the wire
    // tests exercise decrypt + replay-guard + dispatch without a socket.
    static void processIncoming(SatelliteClient& client, const std::uint8_t* buf, std::size_t n) {
        client.processIncoming(buf, n);
    }

    // Direct line to the send framing, for the payload-ceiling tests: the
    // public senders each guard their own payloads before this, so the generic
    // ceiling would otherwise be unreachable at its exact boundary.
    static bool sendEncrypted(SatelliteClient& client, std::uint16_t msgType,
                              const std::uint8_t* payload, std::size_t len) {
        return client.sendEncrypted(msgType, payload, len);
    }
};

} // namespace dish::net
