// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/UsbClaimHealth.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::ClaimActivity;
using dish::reducer::ClaimHealth;
using dish::reducer::judgeClaimHealth;
using dish::reducer::kSilentScansBeforeUnreadable;
using dish::reducer::kUndecodedReadsBeforeUnreadable;

namespace {

ClaimActivity activity(std::int64_t reads, std::int64_t decoded, int scans,
                       bool streamsUnprompted) {
    ClaimActivity a;
    a.reads = reads;
    a.decoded = decoded;
    a.scans = scans;
    a.streamsUnprompted = streamsUnprompted;
    return a;
}

} // namespace

TEST_CASE("a claim that has decoded a report is decoding, whatever else it saw", "[usb-health]") {
    CHECK(judgeClaimHealth(activity(1, 1, 0, false)) == ClaimHealth::Decoding);
    CHECK(judgeClaimHealth(activity(5000, 1, 99, true)) == ClaimHealth::Decoding);
    CHECK(judgeClaimHealth(activity(1, 1, kSilentScansBeforeUnreadable, true)) ==
          ClaimHealth::Decoding);
}

TEST_CASE("reads that never decode are unreadable once there are enough to judge", "[usb-health]") {
    CHECK(judgeClaimHealth(activity(kUndecodedReadsBeforeUnreadable, 0, 0, false)) ==
          ClaimHealth::Unreadable);
    CHECK(judgeClaimHealth(activity(kUndecodedReadsBeforeUnreadable, 0, 0, true)) ==
          ClaimHealth::Unreadable);
    CHECK(judgeClaimHealth(activity(kUndecodedReadsBeforeUnreadable - 1, 0, 0, false)) ==
          ClaimHealth::Undecided);
}

TEST_CASE("a family that streams unprompted is unreadable after enough silent scans",
          "[usb-health]") {
    CHECK(judgeClaimHealth(activity(0, 0, kSilentScansBeforeUnreadable, true)) ==
          ClaimHealth::Unreadable);
    CHECK(judgeClaimHealth(activity(0, 0, kSilentScansBeforeUnreadable - 1, true)) ==
          ClaimHealth::Undecided);
}

TEST_CASE("a family that reports only on change may stay silent forever", "[usb-health]") {
    CHECK(judgeClaimHealth(activity(0, 0, kSilentScansBeforeUnreadable, false)) ==
          ClaimHealth::Undecided);
    CHECK(judgeClaimHealth(activity(0, 0, 1000, false)) == ClaimHealth::Undecided);
}

TEST_CASE("a fresh claim is undecided", "[usb-health]") {
    CHECK(judgeClaimHealth(activity(0, 0, 0, true)) == ClaimHealth::Undecided);
    CHECK(judgeClaimHealth(activity(0, 0, 0, false)) == ClaimHealth::Undecided);
}
