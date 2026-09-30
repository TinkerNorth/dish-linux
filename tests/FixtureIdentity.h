// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Self-signed RSA identities for the loopback TLS fakes, minted once for the
// whole run. The prime search behind a 2048-bit key takes a variable and
// occasionally long time, and neither end of a test handshake gains anything
// from a fresh one per case; an install has exactly one identity anyway.

#pragma once

#include "core/moonlight/MoonlightPairingCrypto.h"

namespace dish::test {

// What a fake server presents.
inline const mooncrypto::ClientIdentity& fixtureHostIdentity() {
    static const mooncrypto::ClientIdentity id =
        mooncrypto::generateClientIdentity().value_or(mooncrypto::ClientIdentity{});
    return id;
}

// What a client under test presents, where the exchange is mutual.
inline const mooncrypto::ClientIdentity& fixtureClientIdentity() {
    static const mooncrypto::ClientIdentity id =
        mooncrypto::generateClientIdentity().value_or(mooncrypto::ClientIdentity{});
    return id;
}

} // namespace dish::test
