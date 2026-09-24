// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/PairingOutcome.h"

#include "core/reducer/RestOutcome.h"

#include <QCoreApplication>

namespace dish::net {

PairingOutcome::Arm PairingOutcome::classify(const models::PairResponse& response,
                                             bool pinMismatch) {
    reducer::PairReply r;
    r.status = response.httpStatus;
    r.bodyParsed = response.reachable;
    r.ok = response.ok;
    r.pending = response.pending;
    r.hasSharedKey = response.sharedKey.has_value() && !response.sharedKey->isEmpty();
    r.pinMismatch = pinMismatch;
    switch (reducer::classifyPair(r)) {
    case reducer::PairVerdict::Success:
        // classifyPair only says Success when hasSharedKey, which is exactly
        // this optional being engaged; value_or keeps that local rather than
        // asking a reader to carry the invariant across two files.
        return Success{response.sharedKey.value_or(QString())};
    case reducer::PairVerdict::Pending:
        return Pending{};
    case reducer::PairVerdict::AuthRequired:
        return AuthRequired{};
    case reducer::PairVerdict::VersionMismatch:
        return VersionMismatch{};
    case reducer::PairVerdict::IdentityChanged:
        return IdentityChanged{};
    case reducer::PairVerdict::Unreachable:
        break;
    }
    return Unreachable{response.error.value_or(
        QCoreApplication::translate("dish::net::PairingOutcome", "Server unreachable"))};
}

} // namespace dish::net
