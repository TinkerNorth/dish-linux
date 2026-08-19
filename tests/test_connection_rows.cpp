// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/ConnectionRows.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace dish::reducer;

TEST_CASE("glyph: satellite Connected uses the connected glyph", "[rows][glyph]") {
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Connected) ==
            ConnectionGlyph::SatelliteConnected);
}

TEST_CASE("glyph: satellite Saved uses the off glyph", "[rows][glyph]") {
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Saved) ==
            ConnectionGlyph::SatelliteOff);
}

TEST_CASE("glyph: satellite Stale uses the off glyph (same as Saved)", "[rows][glyph]") {
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Stale) ==
            ConnectionGlyph::SatelliteOff);
}

TEST_CASE("glyph: satellite Connecting/Found/Ready/Unstable use the base glyph", "[rows][glyph]") {
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Connecting) ==
            ConnectionGlyph::SatelliteBase);
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Found) ==
            ConnectionGlyph::SatelliteBase);
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Ready) ==
            ConnectionGlyph::SatelliteBase);
    REQUIRE(glyphForConnection(ConnectionKind::Satellite, UiLinkState::Unstable) ==
            ConnectionGlyph::SatelliteBase);
}

TEST_CASE("dot: Connected is success green", "[rows][dot]") {
    REQUIRE(dotColorForState(UiLinkState::Connected) == DotColor::Success);
}

TEST_CASE("dot: Connecting and Unstable share primary", "[rows][dot]") {
    REQUIRE(dotColorForState(UiLinkState::Connecting) == DotColor::Primary);
    REQUIRE(dotColorForState(UiLinkState::Unstable) == DotColor::Primary);
}

TEST_CASE("dot: Stale is warning", "[rows][dot]") {
    REQUIRE(dotColorForState(UiLinkState::Stale) == DotColor::Warning);
}

TEST_CASE("dot: Saved/Ready/Found collapse to muted", "[rows][dot]") {
    REQUIRE(dotColorForState(UiLinkState::Saved) == DotColor::Muted);
    REQUIRE(dotColorForState(UiLinkState::Ready) == DotColor::Muted);
    REQUIRE(dotColorForState(UiLinkState::Found) == DotColor::Muted);
}

TEST_CASE("chip: each LinkState maps to its shared vocabulary key", "[rows][chip]") {
    REQUIRE(statusChipKey(UiLinkState::Found) == StatusChipKey::Found);
    REQUIRE(statusChipKey(UiLinkState::Stale) == StatusChipKey::NeedsPairing);
    REQUIRE(statusChipKey(UiLinkState::Saved) == StatusChipKey::Offline);
    REQUIRE(statusChipKey(UiLinkState::Ready) == StatusChipKey::Ready);
    REQUIRE(statusChipKey(UiLinkState::Connecting) == StatusChipKey::Connecting);
    REQUIRE(statusChipKey(UiLinkState::Connected) == StatusChipKey::Online);
    REQUIRE(statusChipKey(UiLinkState::Unstable) == StatusChipKey::Unstable);
}

namespace {
constexpr std::array<UiLinkState, 7> kAllStates = {
    UiLinkState::Found,      UiLinkState::Stale,     UiLinkState::Saved,   UiLinkState::Ready,
    UiLinkState::Connecting, UiLinkState::Connected, UiLinkState::Unstable};
} // namespace

TEST_CASE("rows: every state has a chip, a dot color, and a glyph", "[rows][exhaustive]") {
    for (const auto state : kAllStates) {
        // Every mapper has a default arm, so this only proves totality; the
        // values themselves are pinned in the cases above.
        (void)statusChipKey(state);
        (void)dotColorForState(state);
        (void)glyphForConnection(ConnectionKind::Satellite, state);
    }
    SUCCEED();
}

TEST_CASE("isLiveLink: connected and unstable are live", "[rows][live]") {
    REQUIRE(isLiveLink(UiLinkState::Connected));
    REQUIRE(isLiveLink(UiLinkState::Unstable));
}

TEST_CASE("isLiveLink: every other state is not live", "[rows][live]") {
    REQUIRE_FALSE(isLiveLink(UiLinkState::Connecting));
    REQUIRE_FALSE(isLiveLink(UiLinkState::Ready));
    REQUIRE_FALSE(isLiveLink(UiLinkState::Found));
    REQUIRE_FALSE(isLiveLink(UiLinkState::Saved));
    REQUIRE_FALSE(isLiveLink(UiLinkState::Stale));
}
