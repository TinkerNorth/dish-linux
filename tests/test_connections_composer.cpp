// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "architecture/Observable.h"
#include "composer/ConnectionsComposer.h"
#include "core/reducer/ConnectionRows.h"
#include "core/reducer/SatelliteLinkState.h"

#include "ComposerProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using dish::arch::Observable;
using dish::composer::Binding;
using dish::composer::buildConnectionSummaries;
using dish::composer::ConnectionRow;
using dish::composer::ConnectionsComposer;
using dish::composer::RememberedSnapshot;
using dish::composer::SessionSnapshot;
using dish::reducer::SessionPresence;
using dish::reducer::UiLinkState;
using dish::test::ComposerProbe;

namespace {

SessionSnapshot session(const std::string& id, SessionPresence presence, const std::string& ip,
                        int port = 9876, const std::string& name = {}) {
    SessionSnapshot s;
    s.id = id;
    s.presence = presence;
    s.ip = ip;
    s.udpPort = port;
    s.name = name;
    return s;
}

RememberedSnapshot rememberedSat(const std::string& id, const std::string& ip, int port = 9876,
                                 const std::string& name = {}) {
    RememberedSnapshot r;
    r.id = id;
    r.ip = ip;
    r.udpPort = port;
    r.name = name;
    return r;
}

} // namespace

TEST_CASE("composer transform: empty everything yields no rows", "[composer][transform]") {
    REQUIRE(buildConnectionSummaries({}, {}, {}, {}, {}).empty());
}

TEST_CASE("composer transform: a remembered-only satellite is an Offline (Saved) row",
          "[composer][transform]") {
    const auto rows =
        buildConnectionSummaries({}, {rememberedSat("mid:a", "10.0.0.1", 9876, "Pc")}, {}, {}, {});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].id == "mid:a");
    REQUIRE(rows[0].label == "Pc");
    REQUIRE(rows[0].ip == "10.0.0.1");
    REQUIRE(rows[0].udpPort == 9876);
    REQUIRE(rows[0].live == UiLinkState::Saved);
    REQUIRE(rows[0].boundSlotId.empty());
}

TEST_CASE("composer transform: a discovered remembered-only satellite is Ready",
          "[composer][transform]") {
    const auto rows = buildConnectionSummaries({}, {rememberedSat("mid:a", "10.0.0.1")},
                                               /*discoveredIds=*/{"mid:a"}, {}, {});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].live == UiLinkState::Ready);
}

TEST_CASE("composer transform: a live session row is Connected and carries its bound slot",
          "[composer][transform]") {
    const auto rows =
        buildConnectionSummaries({session("mid:a", SessionPresence::Live, "10.0.0.1", 9876, "Pc")},
                                 {rememberedSat("mid:a", "10.0.0.1", 9876, "Pc")}, {"mid:a"},
                                 /*bindings=*/{{"slot-1", "mid:a"}}, {});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].live == UiLinkState::Connected);
    REQUIRE(rows[0].boundSlotId == "slot-1");
    REQUIRE(rows[0].glyph == dish::reducer::ConnectionGlyph::SatelliteConnected);
    REQUIRE(rows[0].dotColor == dish::reducer::DotColor::Success);
    REQUIRE(rows[0].chip == dish::reducer::StatusChipKey::Online);
}

TEST_CASE("composer transform: a live session's latency readout lands on its row",
          "[composer][transform]") {
    // The readout is a median heartbeat-RTT/2; a remembered-only satellite has
    // no session, so its row keeps the 0 / 0 default.
    auto live = session("mid:a", SessionPresence::Live, "10.0.0.1", 9876, "Pc");
    live.latencyOneWayMs = 3.4;
    live.latencySamples = 64;
    const auto rows = buildConnectionSummaries({live},
                                               {rememberedSat("mid:a", "10.0.0.1", 9876, "Pc"),
                                                rememberedSat("mid:b", "10.0.0.2", 9876, "Tv")},
                                               {}, {}, {});
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].id == "mid:a"); // sorted by label: "Pc" < "Tv"
    REQUIRE(rows[0].latencyOneWayMs == 3.4);
    REQUIRE(rows[0].latencySamples == 64);
    REQUIRE(rows[1].latencyOneWayMs == 0.0);
    REQUIRE(rows[1].latencySamples == 0);
}

TEST_CASE("composer transform: a live session prefers its own (fresher) address over remembered",
          "[composer][transform]") {
    const auto rows =
        buildConnectionSummaries({session("mid:a", SessionPresence::Live, "10.0.0.9", 9876, "Pc")},
                                 {rememberedSat("mid:a", "10.0.0.1", 9876, "Pc")}, {}, {}, {});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].ip == "10.0.0.9");
}

TEST_CASE("composer transform: a stale id lifts an idle remembered satellite to Stale",
          "[composer][transform]") {
    const auto rows = buildConnectionSummaries({}, {rememberedSat("mid:a", "10.0.0.1")}, {}, {},
                                               /*staleIds=*/{"mid:a"});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].live == UiLinkState::Stale);
}

TEST_CASE("composer transform: a stale marker does NOT override a live session",
          "[composer][transform]") {
    const auto rows =
        buildConnectionSummaries({session("mid:a", SessionPresence::Live, "10.0.0.1")}, {}, {}, {},
                                 /*staleIds=*/{"mid:a"});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].live == UiLinkState::Connected);
}

TEST_CASE("composer transform: a live-but-unremembered session still gets a row",
          "[composer][transform]") {
    // Mid-pair, before the first PUT remembers it.
    const auto rows = buildConnectionSummaries(
        {session("mid:a", SessionPresence::Linking, "10.0.0.1")}, {}, {}, {}, {});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].live == UiLinkState::Connecting);
}

TEST_CASE("composer transform: a ghost with no ip is dropped", "[composer][transform]") {
    const auto rows =
        buildConnectionSummaries({session("mid:a", SessionPresence::Idle, "")}, {}, {}, {}, {});
    REQUIRE(rows.empty());
}

TEST_CASE("composer transform: rows are sorted by label then id", "[composer][transform]") {
    const auto rows = buildConnectionSummaries({},
                                               {rememberedSat("mid:z", "10.0.0.3", 9876, "Zeta"),
                                                rememberedSat("mid:a", "10.0.0.1", 9876, "Alpha"),
                                                rememberedSat("mid:b", "10.0.0.2", 9876, "Alpha")},
                                               {}, {}, {});
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0].label == "Alpha");
    REQUIRE(rows[0].id == "mid:a"); // id tiebreak within equal labels
    REQUIRE(rows[1].label == "Alpha");
    REQUIRE(rows[1].id == "mid:b");
    REQUIRE(rows[2].label == "Zeta");
}

TEST_CASE("composer transform: a nameless server labels by ip", "[composer][transform]") {
    const auto rows =
        buildConnectionSummaries({}, {rememberedSat("mid:a", "10.0.0.1")}, {}, {}, {});
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].label == "10.0.0.1");
}

TEST_CASE("ConnectionsComposer derives eagerly: the probe sees an initial snapshot",
          "[composer][reactive]") {
    Observable<std::vector<SessionSnapshot>> sessions{{}};
    Observable<std::vector<RememberedSnapshot>> remembered{{rememberedSat("mid:a", "10.0.0.1")}};
    Observable<std::vector<std::string>> discovered{{}};
    Observable<std::vector<Binding>> bindings{{}};
    Observable<std::vector<std::string>> stale{{}};

    ConnectionsComposer composer(sessions, remembered, discovered, bindings, stale);
    ComposerProbe<std::vector<ConnectionRow>> probe(composer.state());

    REQUIRE(probe.count() == 1);
    REQUIRE(probe.latest().size() == 1);
    REQUIRE(probe.latest()[0].live == UiLinkState::Saved);
}

TEST_CASE("ConnectionsComposer recomputes when a session transitions Linking -> Live",
          "[composer][reactive]") {
    Observable<std::vector<SessionSnapshot>> sessions{
        {session("mid:a", SessionPresence::Linking, "10.0.0.1")}};
    Observable<std::vector<RememberedSnapshot>> remembered{{}};
    Observable<std::vector<std::string>> discovered{{}};
    Observable<std::vector<Binding>> bindings{{}};
    Observable<std::vector<std::string>> stale{{}};

    ConnectionsComposer composer(sessions, remembered, discovered, bindings, stale);
    ComposerProbe<std::vector<ConnectionRow>> probe(composer.state());

    REQUIRE(probe.latest()[0].live == UiLinkState::Connecting);

    sessions.set({session("mid:a", SessionPresence::Live, "10.0.0.1")});

    REQUIRE(probe.count() == 2); // initial + one recompute
    REQUIRE(probe.latest()[0].live == UiLinkState::Connected);
}

TEST_CASE("ConnectionsComposer is distinct-until-changed: a no-op upstream set does not re-emit",
          "[composer][reactive]") {
    Observable<std::vector<SessionSnapshot>> sessions{
        {session("mid:a", SessionPresence::Live, "10.0.0.1")}};
    Observable<std::vector<RememberedSnapshot>> remembered{{}};
    Observable<std::vector<std::string>> discovered{{}};
    Observable<std::vector<Binding>> bindings{{}};
    Observable<std::vector<std::string>> stale{{}};

    ConnectionsComposer composer(sessions, remembered, discovered, bindings, stale);
    ComposerProbe<std::vector<ConnectionRow>> probe(composer.state());

    REQUIRE(probe.count() == 1);
    // Suppressed twice over: the upstream Observable drops the equal set, and
    // the derived list would be identical anyway.
    sessions.set({session("mid:a", SessionPresence::Live, "10.0.0.1")});
    REQUIRE(probe.count() == 1);

    // A discovered id matching no row leaves the derived list unchanged.
    discovered.set({"mid:other"});
    REQUIRE(probe.count() == 1);
}

TEST_CASE("ConnectionsComposer recomputes on a binding change", "[composer][reactive]") {
    Observable<std::vector<SessionSnapshot>> sessions{
        {session("mid:a", SessionPresence::Live, "10.0.0.1")}};
    Observable<std::vector<RememberedSnapshot>> remembered{{}};
    Observable<std::vector<std::string>> discovered{{}};
    Observable<std::vector<Binding>> bindings{{}};
    Observable<std::vector<std::string>> stale{{}};

    ConnectionsComposer composer(sessions, remembered, discovered, bindings, stale);
    ComposerProbe<std::vector<ConnectionRow>> probe(composer.state());

    REQUIRE(probe.latest()[0].boundSlotId.empty());
    bindings.set({{"slot-7", "mid:a"}});
    REQUIRE(probe.count() == 2);
    REQUIRE(probe.latest()[0].boundSlotId == "slot-7");
}

TEST_CASE("ConnectionsComposer recomputes when a stale id arrives for an idle row",
          "[composer][reactive]") {
    Observable<std::vector<SessionSnapshot>> sessions{{}};
    Observable<std::vector<RememberedSnapshot>> remembered{{rememberedSat("mid:a", "10.0.0.1")}};
    Observable<std::vector<std::string>> discovered{{}};
    Observable<std::vector<Binding>> bindings{{}};
    Observable<std::vector<std::string>> stale{{}};

    ConnectionsComposer composer(sessions, remembered, discovered, bindings, stale);
    ComposerProbe<std::vector<ConnectionRow>> probe(composer.state());

    REQUIRE(probe.latest()[0].live == UiLinkState::Saved);
    stale.set({"mid:a"});
    REQUIRE(probe.count() == 2);
    REQUIRE(probe.latest()[0].live == UiLinkState::Stale);
}
