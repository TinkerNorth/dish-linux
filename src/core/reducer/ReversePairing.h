// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The decision core for host-initiated (reverse, Path B) pairing: the dish shows
// a 4-digit clientPin, POSTs it to /api/pair, then polls GET /api/pair/status
// until the operator approves on the satellite. The manager owns the timer and
// the live call and feeds each verdict plus the clock reading through here.

#pragma once

#include "core/reducer/RestOutcome.h"

#include <cstdint>

namespace dish::reducer {

enum class ReversePairingAction {
    Approve,     // approved with a usable key: adopt and open the session
    Decline,     // operator denied; terminal
    KeepPolling, // pending or momentarily unreachable, and time remains
    TimeOut,     // no resolution by the deadline
};

// Total over every ApprovalVerdict x clock reading. Declined is terminal
// regardless of the clock: a denied grant never becomes approved, so stop rather
// than burning the deadline. Approved is safe to act on because classifyApproval
// degrades a keyless "approved" to Pending upstream. The deadline is INCLUSIVE:
// at elapsed == deadlineMs the budget is spent, so one more poll would overrun.
inline ReversePairingAction nextReversePairingAction(ApprovalVerdict approval,
                                                     std::int64_t elapsedMs,
                                                     std::int64_t deadlineMs) {
    switch (approval) {
    case ApprovalVerdict::Approved:
        return ReversePairingAction::Approve;
    case ApprovalVerdict::Declined:
        return ReversePairingAction::Decline;
    case ApprovalVerdict::Pending:
    case ApprovalVerdict::Unreachable:
        return (elapsedMs >= deadlineMs) ? ReversePairingAction::TimeOut
                                         : ReversePairingAction::KeepPolling;
    }
    // The switch is exhaustive; only a bogus cast lands here.
    return (elapsedMs >= deadlineMs) ? ReversePairingAction::TimeOut
                                     : ReversePairingAction::KeepPolling;
}

// ── 4-digit PIN shape ────────────────────────────────────────────────────────
// The value is random, but the contract fixes the shape at exactly 4 zero-padded
// decimal digits. Formatter and validator live here so a test can pin the shape
// without depending on randomness.

inline constexpr int kReversePinDigits = 4;
inline constexpr int kReversePinModulus = 10000; // 10^kReversePinDigits

// Reduced modulo 10000 so any source, such as a 32-bit random draw, lands in
// range and the output is always exactly 4 digits.
inline std::string formatReversePin(std::uint32_t raw) {
    const unsigned value = static_cast<unsigned>(raw % kReversePinModulus);
    std::string out(kReversePinDigits, '0');
    unsigned v = value;
    for (int i = kReversePinDigits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    return out;
}

// Rejects a malformed displayed PIN before it reaches the wire, where the
// server's Path-B compare expects exactly 4 ASCII decimal digits.
inline bool isValidReversePin(const std::string& pin) {
    if (pin.size() != static_cast<std::size_t>(kReversePinDigits)) { return false; }
    for (const char c : pin) {
        if (c < '0' || c > '9') { return false; }
    }
    return true;
}

} // namespace dish::reducer
