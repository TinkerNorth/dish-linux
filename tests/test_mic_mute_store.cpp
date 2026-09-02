// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MicMuteStore (source/store/MicMuteStore.h): live per-slot mute, deliberately
// session-scoped. Unlike the toggle stores next to it there is no repository
// here at all — that absence is the design (a mute that survived a restart
// would hide a silenced microphone behind a fresh-looking session), so the
// suite pins behaviour on a bare store.

#include "source/store/MicMuteStore.h"

#include <catch2/catch_test_macros.hpp>

using dish::source::MicMuteStore;

TEST_CASE("a slot nobody muted is live", "[mic-mute]") {
    MicMuteStore store;
    CHECK_FALSE(store.isMuted("9"));
    CHECK_FALSE(MicMuteStore::kDefaultMuted);
    CHECK(store.state().value().empty());
}

TEST_CASE("setMuted holds and clears per slot", "[mic-mute]") {
    MicMuteStore store;
    store.setMuted("9", true);
    CHECK(store.isMuted("9"));
    CHECK_FALSE(store.isMuted("other"));
    store.setMuted("9", false);
    CHECK_FALSE(store.isMuted("9"));
}

TEST_CASE("toggle flips and reports the new state", "[mic-mute]") {
    // Both mute buttons are presses over one toggle, so the return value is
    // what the caller renders without a second read.
    MicMuteStore store;
    CHECK(store.toggle("9"));
    CHECK(store.isMuted("9"));
    CHECK_FALSE(store.toggle("9"));
    CHECK_FALSE(store.isMuted("9"));
}

TEST_CASE("a redundant write does not republish", "[mic-mute]") {
    MicMuteStore store;
    store.setMuted("9", true);
    int emits = 0;
    auto sub = store.state().subscribe([&emits](const dish::source::MicMuteMap&) { emits++; },
                                       /*emitCurrent=*/false);
    store.setMuted("9", true); // no change, no emit
    CHECK(emits == 0);
    store.setMuted("9", false);
    CHECK(emits == 1);
}

TEST_CASE("retainOnly drops departed slots so a replug comes back live", "[mic-mute]") {
    // A physical pad's slot id is model-keyed and REUSED across replugs; the
    // hardware clears its own mute at power-down, and the app must match it.
    MicMuteStore store;
    store.setMuted("gone", true);
    store.setMuted("here", true);
    store.retainOnly({"here"});
    CHECK(store.isMuted("here"));
    CHECK_FALSE(store.isMuted("gone"));
    const auto snapshot = store.state().value();
    CHECK(snapshot.find("gone") == snapshot.end());
}
