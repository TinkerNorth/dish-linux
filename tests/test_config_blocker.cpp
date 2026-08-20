// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Precedence: nothing until loaded; controller loss beats everything; no host
// selected means no host blocker; a missing or non-live host is HostLost (label
// from the summary, else the remembered map, else empty); an Unstable host is
// HostUnsteady unless dismissed for that host.

#include "core/reducer/ConfigBlocker.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using dish::reducer::BlockerHostRow;
using dish::reducer::ConfigBlocker;
using dish::reducer::configBlockerFor;
using dish::reducer::ConfigBlockerKind;
using dish::reducer::isLiveLink;
using dish::reducer::UiLinkState;

namespace {

BlockerHostRow summary(const std::string& id, UiLinkState live, const std::string& label = {}) {
    return BlockerHostRow{id, label.empty() ? id : label, live};
}

struct Args {
    bool loaded = true;
    bool controllerPresent = true;
    std::optional<std::string> hostId = std::string("s:1");
    std::vector<BlockerHostRow> connections;
    std::map<std::string, std::string> knownHostLabels;
    std::set<std::string> dismissed;
};

ConfigBlocker blockerOf(const Args& a) {
    return configBlockerFor(a.loaded, a.controllerPresent, a.hostId, a.connections,
                            a.knownHostLabels, a.dismissed);
}

ConfigBlocker hostLost(const std::string& label, bool reconnecting) {
    return ConfigBlocker{ConfigBlockerKind::HostLost, label, reconnecting};
}

const ConfigBlocker kUnsteady{ConfigBlockerKind::HostUnsteady, {}, false};

} // namespace

TEST_CASE("ConfigBlocker: no blocker before load completes", "[config-blocker]") {
    Args a;
    a.loaded = false;
    a.controllerPresent = false; // even a lost controller must not flash a blocker
    CHECK(blockerOf(a) == ConfigBlocker{});
}

TEST_CASE("ConfigBlocker: controller loss blocks regardless of host state", "[config-blocker]") {
    Args a;
    a.controllerPresent = false;
    a.connections = {summary("s:1", UiLinkState::Connected)};
    CHECK(blockerOf(a).kind == ConfigBlockerKind::InputLost);
}

TEST_CASE("ConfigBlocker: no host selected means no host blocker", "[config-blocker]") {
    Args a;
    a.hostId = std::nullopt;
    CHECK(blockerOf(a) == ConfigBlocker{});
}

TEST_CASE("ConfigBlocker: connected host is not blocked", "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Connected)};
    CHECK(blockerOf(a) == ConfigBlocker{});
}

TEST_CASE("ConfigBlocker: offline host blocks as lost", "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Saved, "Den PC")};
    CHECK(blockerOf(a) == hostLost("Den PC", /*reconnecting=*/false));
}

TEST_CASE("ConfigBlocker: connecting host blocks as lost with reconnect in flight",
          "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Connecting, "Den PC")};
    CHECK(blockerOf(a) == hostLost("Den PC", /*reconnecting=*/true));
}

TEST_CASE("ConfigBlocker: forgotten host falls back to the last known label", "[config-blocker]") {
    Args a; // no connection row at all for s:1
    a.knownHostLabels = {{"s:1", "Den PC"}};
    CHECK(blockerOf(a) == hostLost("Den PC", /*reconnecting=*/false));
}

TEST_CASE("ConfigBlocker: never seen host blocks with an empty label", "[config-blocker]") {
    CHECK(blockerOf(Args{}) == hostLost("", /*reconnecting=*/false));
}

TEST_CASE("ConfigBlocker: unstable host blocks as unsteady", "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Unstable)};
    CHECK(blockerOf(a) == kUnsteady);
}

TEST_CASE("ConfigBlocker: dismissed unsteady warning stays dismissed", "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Unstable)};
    a.dismissed = {"s:1"};
    CHECK(blockerOf(a) == ConfigBlocker{});
}

TEST_CASE("ConfigBlocker: dismissing unsteady does not suppress a real loss", "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Saved)}; // label defaults to the id
    a.dismissed = {"s:1"};
    CHECK(blockerOf(a) == hostLost("s:1", /*reconnecting=*/false));
}

TEST_CASE("ConfigBlocker: unrelated dismissal does not hide the unsteady warning",
          "[config-blocker]") {
    Args a;
    a.connections = {summary("s:1", UiLinkState::Unstable)};
    a.dismissed = {"s:other"};
    CHECK(blockerOf(a) == kUnsteady);
}

TEST_CASE("ConfigBlocker: every link state resolves to a deterministic blocker",
          "[config-blocker]") {
    const UiLinkState all[] = {
        UiLinkState::Found,      UiLinkState::Stale,     UiLinkState::Saved,   UiLinkState::Ready,
        UiLinkState::Connecting, UiLinkState::Connected, UiLinkState::Unstable};
    for (const auto link : all) {
        CAPTURE(static_cast<int>(link));
        Args a;
        a.connections = {summary("s:1", link)};
        const auto b = blockerOf(a);
        if (link == UiLinkState::Connected) {
            CHECK(b == ConfigBlocker{});
        } else if (link == UiLinkState::Unstable) {
            CHECK(b == kUnsteady);
        } else {
            CHECK(b.kind == ConfigBlockerKind::HostLost);
        }
    }
}

TEST_CASE("ConfigBlocker: isLiveLink is true only for Connected and Unstable", "[config-blocker]") {
    CHECK(isLiveLink(UiLinkState::Connected));
    CHECK(isLiveLink(UiLinkState::Unstable));
    CHECK_FALSE(isLiveLink(UiLinkState::Found));
    CHECK_FALSE(isLiveLink(UiLinkState::Stale));
    CHECK_FALSE(isLiveLink(UiLinkState::Saved));
    CHECK_FALSE(isLiveLink(UiLinkState::Ready));
    CHECK_FALSE(isLiveLink(UiLinkState::Connecting));
}
