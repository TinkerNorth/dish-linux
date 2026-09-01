// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Protocol version negotiation, the client half of satellite/docs/contract.md
// "Versioning".
//
// The client offers proto::kProtocolVersion on pairing and the session PUT. The
// satellite accepts its whole [supportedMin, supported] range, settles the
// session on the client's offer and echoes the settled version; an offer
// outside the range comes back 409 with the two bounds so the rejected side
// knows which end must update.
//
// Two facts make this more than a comparison:
//
//   - A pre-versioning satellite does not answer 409. It ignores the offer and
//     echoes 1, so the SETTLED version, not the offered one, is what keys the
//     wire frames. Reading the echo as "whatever we asked for" would make a v2
//     client send 19-byte POINTER frames to a server that only decodes 16.
//   - A 409 is not automatically terminal. When the two ranges still overlap,
//     the highest common version is a working session, so the client re-offers
//     it instead of dead-ending the user on "update something".

#pragma once

#include "core/model/Protocol.h"

#include <cstdint>

namespace dish::reducer {

enum class ProtocolVerdict : std::uint8_t {
    // The session speaks `settledVersion`; the wire frames key off it.
    Settled,
    // 409, but the ranges overlap: re-offer `settledVersion` and try again.
    RetryLower,
    // The satellite's floor is above what this client speaks: update Dish.
    UpdateDish,
    // The satellite's ceiling is below this client's floor: update the satellite.
    UpdateSatellite,
    // A 409 whose bounds are missing or nonsensical. Neither end can be blamed
    // from the wire, so the caller surfaces a plain version-mismatch and does
    // NOT invent a direction.
    Unusable,
};

struct ProtocolOutcome {
    ProtocolVerdict verdict = ProtocolVerdict::Settled;
    // Meaningful for Settled (the version in force) and RetryLower (the version
    // to offer next). Left at the client's own version otherwise.
    int settledVersion = proto::kProtocolVersion;
    // Settled only, and never an error: the satellite works but is older than
    // this client, so the UI shows the soft "update for the newest features"
    // hint. The mirror of the dashboard's Update Dish chip.
    bool satelliteBehind = false;
};

// The 200 path. `echoedVersion` is the response's `protocolVersion`; the caller
// passes proto::kProtocolVersionMin when the field is absent, which is what the
// contract says an omitted field means.
//
// An echo ABOVE our offer is a server that did not honor the contract's "settle
// on the client's offer". Clamping down to our own version rather than trusting
// it keeps us on frames we can actually encode.
inline ProtocolOutcome settleAccepted(int echoedVersion) {
    ProtocolOutcome out;
    out.verdict = ProtocolVerdict::Settled;
    if (echoedVersion >= proto::kProtocolVersion) {
        out.settledVersion = proto::kProtocolVersion;
        out.satelliteBehind = false;
        return out;
    }
    // Below our floor is a satellite older than anything we can speak; the
    // floor is the oldest frame set we still encode, so clamp there and let the
    // session fail on its own terms rather than silently mis-framing.
    out.settledVersion =
        echoedVersion < proto::kProtocolVersionMin ? proto::kProtocolVersionMin : echoedVersion;
    out.satelliteBehind = true;
    return out;
}

// The 409 path. `supported` / `supportedMin` come from the error body; pass 0
// for a field the body did not carry.
inline ProtocolOutcome settleRejected(int supported, int supportedMin) {
    ProtocolOutcome out;
    // A body that carries no usable range, or an inverted one, cannot say which
    // end is behind.
    if (supported <= 0 || supportedMin <= 0 || supportedMin > supported) {
        out.verdict = ProtocolVerdict::Unusable;
        return out;
    }
    if (supportedMin > proto::kProtocolVersion) {
        out.verdict = ProtocolVerdict::UpdateDish;
        return out;
    }
    if (supported < proto::kProtocolVersionMin) {
        out.verdict = ProtocolVerdict::UpdateSatellite;
        return out;
    }
    // The ranges overlap. The highest common version is `supported`, since a
    // 409 for our offer means `supported` is below it.
    out.verdict = ProtocolVerdict::RetryLower;
    out.settledVersion = supported < proto::kProtocolVersion ? supported : proto::kProtocolVersion;
    return out;
}

// Whether a rejection ends the attempt. RetryLower does not: the caller re-PUTs
// at the lower offer.
inline bool protocolVerdictTerminal(ProtocolVerdict v) {
    return v == ProtocolVerdict::UpdateDish || v == ProtocolVerdict::UpdateSatellite ||
           v == ProtocolVerdict::Unusable;
}

} // namespace dish::reducer
