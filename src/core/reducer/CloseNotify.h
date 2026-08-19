// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Maps a SESSION_CLOSE (0x000F) reason byte to the session FSM's follow-up
// action. See satellite/docs/contract.md, Session-close notify.

#pragma once

#include "core/model/Protocol.h"

#include <cstdint>
#include <string_view>

namespace dish::reducer {

enum class CloseAction {
    DropKeyRePair, // trust revoked: drop the key, surface re-pair, stop retrying
    StayDown,      // a newer PUT already owns the session
    RetryBackoff,  // transient: reconnect on the backoff curve
};

inline CloseAction closeActionForReason(std::uint8_t reason) {
    switch (reason) {
    case proto::kCloseReasonUnpaired:
        return CloseAction::DropKeyRePair;
    case proto::kCloseReasonReplaced:
        return CloseAction::StayDown;
    case proto::kCloseReasonShutdown:
    case proto::kCloseReasonKicked:
    default:
        return CloseAction::RetryBackoff;
    }
}

// Protocol constants, never localized.
inline std::string_view closeReasonName(std::uint8_t reason) {
    switch (reason) {
    case proto::kCloseReasonShutdown:
        return "shutdown";
    case proto::kCloseReasonKicked:
        return "kicked";
    case proto::kCloseReasonReplaced:
        return "replaced";
    case proto::kCloseReasonUnpaired:
        return "unpaired";
    default:
        return "shutdown";
    }
}

} // namespace dish::reducer
