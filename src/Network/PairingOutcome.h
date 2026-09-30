// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QString>

#include <variant>

namespace dish::net {

// What one pairing reply means, as the arm the manager visits.
//
// The exchange itself (POST /api/pair and GET /api/pair/status) goes through
// HTTPClient like every other call to the satellite, so it passes the same TOFU
// pin gate on the same thread. Only the reading of the reply is its own concern.
struct PairingOutcome {
    // Arms map 1:1 onto reducer::PairVerdict; the success arm carries the shared
    // key directly.
    struct Success {
        QString sharedKeyHex;
    };
    struct Pending {};         // Path B accepted - poll /api/pair/status
    struct AuthRequired {};    // reachable, no key - first-time pair, or it forgot us
    struct VersionMismatch {}; // 409 - protocol skew, terminal
    struct IdentityChanged {}; // TOFU pin mismatch - terminal, forget and pair again
    struct Unreachable {
        QString message;
    };
    using Arm =
        std::variant<Success, Pending, AuthRequired, VersionMismatch, Unreachable, IdentityChanged>;

    // `pinMismatch` rides beside the reply because the TOFU gate aborts before any
    // body arrives: a changed certificate and a dead link would otherwise read
    // the same.
    static Arm classify(const models::PairResponse& response, bool pinMismatch = false);
};

} // namespace dish::net
