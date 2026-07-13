// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure mapping from a SESSION_CLOSE (0x000F) reason byte to the session's
// follow-up action (contract §Session-close notify). Ported from dish-windows
// core/reducer/CloseNotify.h. Free function, Qt-free.

#pragma once

#include "Models/Protocol.h"

#include <cstdint>
#include <string_view>

namespace dish::reducer {

// What the session layer does after an authenticated close-notify.
enum class CloseAction {
    DropKeyRePair, // unpaired: trust revoked — drop the stored key, surface re-pair, STOP retrying
    StayDown,      // replaced: a newer PUT already owns the session — do nothing further
    RetryBackoff,  // shutdown / kicked: transient — reconnect on the exponential backoff curve
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

// Lowercase wire name for a close reason (diagnostics/UI cue; protocol constant).
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
