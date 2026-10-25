// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Whether a Direct claim is reading its pad, judged by the coordinator once per scan from the
// gateway's read and decode counts. An Unreadable claim is released so the pad falls back to
// Standard instead of streaming nothing from a slot that looks live.

#pragma once

#include <cstdint>

namespace dish::reducer {

enum class ClaimHealth : std::uint8_t { Undecided, Decoding, Unreadable };

inline constexpr std::int64_t kUndecodedReadsBeforeUnreadable = 8;
inline constexpr int kSilentScansBeforeUnreadable = 3;

struct ClaimActivity {
    std::int64_t reads = 0;
    std::int64_t decoded = 0;
    int scans = 0;
    // The pad reports whether or not anything moved, so silence is a fault rather than idleness.
    bool streamsUnprompted = false;
};

inline ClaimHealth judgeClaimHealth(const ClaimActivity& a) {
    if (a.decoded > 0) { return ClaimHealth::Decoding; }
    if (a.reads >= kUndecodedReadsBeforeUnreadable) { return ClaimHealth::Unreadable; }
    if (a.streamsUnprompted && a.scans >= kSilentScansBeforeUnreadable) {
        return ClaimHealth::Unreadable;
    }
    return ClaimHealth::Undecided;
}

} // namespace dish::reducer
