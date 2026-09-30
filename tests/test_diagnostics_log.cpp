// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The flight recorder's diff. The cases follow dish-android's
// DiagnosticsLogDiffTest, plus the bind and unbind lines this client adds.

#include "core/reducer/DiagnosticsLog.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using dish::reducer::appendBounded;
using dish::reducer::DiagnosticsEvent;
using dish::reducer::DiagnosticsEventKind;
using dish::reducer::DirectClaimFailure;
using dish::reducer::linkEvents;
using dish::reducer::LinkSnapshot;
using dish::reducer::padEvents;
using dish::reducer::PadPath;
using dish::reducer::PadSnapshot;
using dish::reducer::UiLinkState;
using dish::reducer::UsbPhase;

namespace {

constexpr std::int64_t kAt = 1'700'000'000'000;

LinkSnapshot link(const std::string& id, UiLinkState state, const std::string& label = "Desk PC") {
    return {id, label, state};
}

PadSnapshot pad(const std::string& id, PadPath path = PadPath::UsbStandard) {
    PadSnapshot p;
    p.id = id;
    p.name = "DualSense";
    p.path = path;
    return p;
}

PadSnapshot withPhase(PadSnapshot p, UsbPhase phase) {
    p.phase = phase;
    return p;
}

PadSnapshot withFailure(PadSnapshot p, DirectClaimFailure failure) {
    p.failure = failure;
    return p;
}

PadSnapshot boundTo(PadSnapshot p, const std::string& hostId, const std::string& hostLabel) {
    p.boundId = hostId;
    p.boundLabel = hostLabel;
    return p;
}

std::vector<DiagnosticsEventKind> kinds(const std::vector<DiagnosticsEvent>& events) {
    std::vector<DiagnosticsEventKind> out;
    for (const auto& e : events) { out.push_back(e.kind); }
    return out;
}

DiagnosticsEvent logged(DiagnosticsEventKind kind, std::int64_t atMs) {
    DiagnosticsEvent e;
    e.kind = kind;
    e.atMs = atMs;
    return e;
}

} // namespace

TEST_CASE("a link seen for the first time logs that it appeared, in its state",
          "[diagnostics][log]") {
    const auto events = linkEvents({}, {link("s:1", UiLinkState::Connected)}, kAt);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == DiagnosticsEventKind::LinkAppeared);
    CHECK(events[0].subject == "Desk PC");
    CHECK(events[0].to == UiLinkState::Connected);
    CHECK(events[0].atMs == kAt);
}

TEST_CASE("a link whose state moves logs where it came from and where it went",
          "[diagnostics][log]") {
    const auto events = linkEvents({link("s:1", UiLinkState::Connected)},
                                   {link("s:1", UiLinkState::Unstable)}, kAt);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == DiagnosticsEventKind::LinkChanged);
    CHECK(events[0].from == UiLinkState::Connected);
    CHECK(events[0].to == UiLinkState::Unstable);
}

TEST_CASE("a link that is gone logs one removal", "[diagnostics][log]") {
    const auto events = linkEvents({link("s:1", UiLinkState::Connected)}, {}, kAt);
    REQUIRE(kinds(events) == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::LinkRemoved});
    CHECK(events[0].subject == "Desk PC");
}

TEST_CASE("a relabelled link in the same state logs nothing", "[diagnostics][log]") {
    const auto events = linkEvents({link("s:1", UiLinkState::Connected, "Desk PC")},
                                   {link("s:1", UiLinkState::Connected, "Desk PC (2)")}, kAt);
    REQUIRE(events.empty());
}

TEST_CASE("unchanged snapshots log nothing", "[diagnostics][log]") {
    const std::vector<LinkSnapshot> links{link("s:1", UiLinkState::Connected)};
    REQUIRE(linkEvents(links, links, kAt).empty());
    const std::vector<PadSnapshot> pads{boundTo(pad("sdl:1"), "mid:desk", "Desk PC")};
    REQUIRE(padEvents(pads, pads, kAt).empty());
}

TEST_CASE("an attached pad logs the path it came in on", "[diagnostics][log]") {
    const auto events = padEvents(
        {}, {pad("sdl:1"), pad("usb:1", PadPath::UsbDirect), pad("sdl:2", PadPath::Bluetooth)},
        kAt);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == DiagnosticsEventKind::PadAttached);
    CHECK(events[0].subject == "DualSense");
    CHECK(events[0].path == PadPath::UsbStandard);
    CHECK(events[1].path == PadPath::UsbDirect);
    CHECK(events[2].path == PadPath::Bluetooth);
}

TEST_CASE("a detached pad logs one removal", "[diagnostics][log]") {
    const auto events = padEvents({pad("sdl:1")}, {}, kAt);
    REQUIRE(kinds(events) == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadDetached});
}

TEST_CASE("a pad's replug, stuck-restore and failed-claim onsets log once each",
          "[diagnostics][log]") {
    const PadSnapshot before = pad("usb:1", PadPath::UsbDirect);

    const auto replug = padEvents({before}, {withPhase(before, UsbPhase::NeedsReplug)}, kAt);
    REQUIRE(kinds(replug) ==
            std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadNeedsReplug});

    const auto stuck = padEvents({before}, {withPhase(before, UsbPhase::RestoreStuck)}, kAt);
    REQUIRE(kinds(stuck) ==
            std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadRestoreStuck});

    const auto failed =
        padEvents({before}, {withFailure(before, DirectClaimFailure::PermissionDenied)}, kAt);
    REQUIRE(kinds(failed) ==
            std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadDirectFailed});
    CHECK(failed[0].failure == DirectClaimFailure::PermissionDenied);
}

TEST_CASE("a flag that stays set logs nothing on the next snapshot", "[diagnostics][log]") {
    const PadSnapshot flagged =
        withFailure(withPhase(pad("usb:1"), UsbPhase::NeedsReplug), DirectClaimFailure::Busy);
    REQUIRE(padEvents({flagged}, {flagged}, kAt).empty());
    const PadSnapshot stuck = withPhase(pad("usb:1"), UsbPhase::RestoreStuck);
    REQUIRE(padEvents({stuck}, {stuck}, kAt).empty());
}

TEST_CASE("a flag that clears logs nothing either", "[diagnostics][log]") {
    const PadSnapshot flagged =
        withFailure(withPhase(pad("usb:1"), UsbPhase::NeedsReplug), DirectClaimFailure::Busy);
    REQUIRE(padEvents({flagged}, {pad("usb:1")}, kAt).empty());
}

TEST_CASE("a pad bound, moved to another host and unbound logs each step", "[diagnostics][log]") {
    const PadSnapshot loose = pad("sdl:1");
    const PadSnapshot desk = boundTo(loose, "mid:desk", "Desk PC");
    const PadSnapshot couch = boundTo(loose, "mid:couch", "Couch PC");

    const auto bound = padEvents({loose}, {desk}, kAt);
    REQUIRE(kinds(bound) == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadBound});
    CHECK(bound[0].host == "Desk PC");

    const auto moved = padEvents({desk}, {couch}, kAt);
    REQUIRE(kinds(moved) == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadBound});
    CHECK(moved[0].host == "Couch PC");

    const auto released = padEvents({couch}, {loose}, kAt);
    REQUIRE(kinds(released) == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::PadUnbound});
    CHECK(released[0].host == "Couch PC");
}

TEST_CASE("a bound host that only changes its label logs no bind", "[diagnostics][log]") {
    const PadSnapshot byAddress = boundTo(pad("sdl:1"), "mid:desk", "192.168.1.20");
    const PadSnapshot byName = boundTo(pad("sdl:1"), "mid:desk", "Desk PC");
    REQUIRE(padEvents({byAddress}, {byName}, kAt).empty());
}

TEST_CASE("the log keeps every event while it is under capacity", "[diagnostics][log]") {
    const std::vector<DiagnosticsEvent> log{logged(DiagnosticsEventKind::LinkAppeared, 1),
                                            logged(DiagnosticsEventKind::LinkChanged, 2)};
    const auto roomy = appendBounded(log, {logged(DiagnosticsEventKind::LinkRemoved, 3)}, 5);
    REQUIRE(roomy.size() == 3);
    CHECK(roomy.front().atMs == 1);
    CHECK(roomy.back().atMs == 3);
}

TEST_CASE("past its capacity the log drops its oldest events first", "[diagnostics][log]") {
    const std::vector<DiagnosticsEvent> log{logged(DiagnosticsEventKind::LinkAppeared, 1),
                                            logged(DiagnosticsEventKind::LinkChanged, 2)};
    const auto full = appendBounded(log, {logged(DiagnosticsEventKind::LinkRemoved, 3)}, 2);
    REQUIRE(full.size() == 2);
    CHECK(full.front().atMs == 2);
    CHECK(full.back().atMs == 3);
}
