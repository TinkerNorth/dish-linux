// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Round-trips a pairing key through the real Secret Service.
//
// SKIPPED wherever no keyring answers — CI runners, containers, headless boxes —
// because the fallback path is what runs there and it is covered by the
// repository's own tests. On a developer desktop with gnome-keyring or KWallet
// this is the only thing that exercises the D-Bus marshalling for real; a
// hand-written (oayays) struct is exactly the sort of thing that compiles
// cleanly and then returns garbage.
//
// Uses an id no real satellite can have, and deletes it again, so a failed run
// cannot leave litter in a developer's keyring.

#include "source/system/SecretServiceStore.h"

#include <catch2/catch_test_macros.hpp>

using namespace dish::source;

namespace {
// One id per test case. ctest runs the suite with --parallel, and a shared id
// would have the cases racing each other over a single keyring item — which is
// exactly how this first failed.
const QString kRoundTripId = QStringLiteral("dish-selftest:do-not-use:round-trip");
const QString kReplaceId = QStringLiteral("dish-selftest:do-not-use:replace");
const QString kSecret = QStringLiteral("00112233445566778899aabbccddeeff"
                                       "00112233445566778899aabbccddeeff");
} // namespace

TEST_CASE("a pairing key round-trips through the desktop keyring") {
    SecretServiceStore store;
    if (!store.available()) { SKIP("no Secret Service on this session bus"); }

    // Clean any residue from an interrupted earlier run before asserting.
    store.erase(kRoundTripId);
    REQUIRE_FALSE(store.read(kRoundTripId).has_value());

    REQUIRE(store.write(kRoundTripId, kSecret));

    const auto readBack = store.read(kRoundTripId);
    REQUIRE(readBack.has_value());
    REQUIRE(*readBack == kSecret);

    REQUIRE(store.erase(kRoundTripId));
    REQUIRE_FALSE(store.read(kRoundTripId).has_value());
}

TEST_CASE("writing the same id twice replaces rather than duplicates") {
    SecretServiceStore store;
    if (!store.available()) { SKIP("no Secret Service on this session bus"); }
    store.erase(kReplaceId);

    REQUIRE(store.write(kReplaceId, kSecret));
    const QString rotated = QStringLiteral("ffffffffffffffffffffffffffffffff"
                                           "ffffffffffffffffffffffffffffffff");
    REQUIRE(store.write(kReplaceId, rotated));

    // A re-pair must not leave the old key discoverable behind the new one.
    const auto readBack = store.read(kReplaceId);
    REQUIRE(readBack.has_value());
    REQUIRE(*readBack == rotated);

    store.erase(kReplaceId);
}

TEST_CASE("an absent id reads as nullopt, and erasing it still succeeds") {
    SecretServiceStore store;
    if (!store.available()) { SKIP("no Secret Service on this session bus"); }
    const QString absent = QStringLiteral("dish-selftest:never-written:0000");
    REQUIRE_FALSE(store.read(absent).has_value());
    // Idempotent: "already gone" is the requested end state, not a failure.
    REQUIRE(store.erase(absent));
}
