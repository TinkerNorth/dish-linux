// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The recorder in front of the flight recorder's diff: what it reads off the
// connection rows and the slot list, and when it writes a line.

#include "Models/Models.h"
#include "architecture/Observable.h"
#include "composer/ConnectionsComposer.h"
#include "composer/DiagnosticsRecorder.h"
#include "core/reducer/DiagnosticsLog.h"
#include "source/store/DiagnosticsLogStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

using dish::arch::Observable;
using dish::composer::boundHostLabelOf;
using dish::composer::ConnectionRow;
using dish::composer::DiagnosticsRecorder;
using dish::composer::padSnapshotOf;
using dish::models::ConnectionSummary;
using dish::models::ControllerSlot;
using dish::reducer::DiagnosticsEventKind;
using dish::reducer::PadPath;
using dish::reducer::UiLinkState;
using dish::reducer::UsbPhase;
using dish::source::DiagnosticsLogStore;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000;

std::int64_t fixedClock() { return kNow; }

ConnectionRow row(UiLinkState live, double latencyMs = 0.0) {
    ConnectionRow r;
    r.id = "mid:desk";
    r.label = "Desk PC";
    r.live = live;
    r.latencyOneWayMs = latencyMs;
    return r;
}

ControllerSlot slot(const QString& id) {
    ControllerSlot s;
    s.id = id;
    s.name = QStringLiteral("DualSense");
    return s;
}

ControllerSlot boundSlot(const QString& id, const QString& hostId, const QString& hostLabel) {
    ControllerSlot s = slot(id);
    s.boundConnectionId = hostId;
    ConnectionSummary status;
    status.id = hostId;
    status.label = hostLabel;
    s.boundStatus = status;
    return s;
}

struct RecorderFixture {
    Observable<std::vector<ConnectionRow>> rows{
        std::vector<ConnectionRow>{row(UiLinkState::Connected)}};
    DiagnosticsLogStore log;
    DiagnosticsRecorder recorder{rows, &log, &fixedClock};

    std::vector<DiagnosticsEventKind> kinds() const {
        std::vector<DiagnosticsEventKind> out;
        for (const auto& e : log.state().value()) { out.push_back(e.kind); }
        return out;
    }
};

} // namespace

TEST_CASE("the recorder logs the links it first sees as appearing, stamped by its clock",
          "[diagnostics][recorder]") {
    const RecorderFixture f;
    const auto events = f.log.state().value();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == DiagnosticsEventKind::LinkAppeared);
    CHECK(events[0].subject == "Desk PC");
    CHECK(events[0].to == UiLinkState::Connected);
    CHECK(events[0].atMs == kNow);
}

TEST_CASE("a link republished for a latency tick logs nothing", "[diagnostics][recorder]") {
    RecorderFixture f;
    f.rows.set({row(UiLinkState::Connected, 3.4)});
    REQUIRE(f.kinds() == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::LinkAppeared});
}

TEST_CASE("a link's republished state change is logged", "[diagnostics][recorder]") {
    RecorderFixture f;
    f.rows.set({row(UiLinkState::Unstable)});
    REQUIRE(f.kinds() == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::LinkAppeared,
                                                           DiagnosticsEventKind::LinkChanged});
}

TEST_CASE("each rebuilt slot list is diffed against the one before it", "[diagnostics][recorder]") {
    RecorderFixture f;
    f.recorder.observeSlots({slot(QStringLiteral("sdl:1"))});
    ControllerSlot replug = slot(QStringLiteral("sdl:1"));
    replug.pathPhase = UsbPhase::NeedsReplug;
    f.recorder.observeSlots({replug});
    f.recorder.observeSlots({replug});
    REQUIRE(f.kinds() == std::vector<DiagnosticsEventKind>{DiagnosticsEventKind::LinkAppeared,
                                                           DiagnosticsEventKind::PadAttached,
                                                           DiagnosticsEventKind::PadNeedsReplug});
}

TEST_CASE("a slot's path reads from its transport", "[diagnostics][recorder]") {
    ControllerSlot direct = slot(QStringLiteral("usb:054c:0ce6"));
    direct.usbDirect = true;
    ControllerSlot wireless = slot(QStringLiteral("sdl:2"));
    wireless.bluetooth = true;
    CHECK(padSnapshotOf(slot(QStringLiteral("sdl:1"))).path == PadPath::UsbStandard);
    CHECK(padSnapshotOf(direct).path == PadPath::UsbDirect);
    CHECK(padSnapshotOf(wireless).path == PadPath::Bluetooth);
}

TEST_CASE("a bound slot carries its host's id and label", "[diagnostics][recorder]") {
    const auto pad = padSnapshotOf(
        boundSlot(QStringLiteral("sdl:1"), QStringLiteral("mid:desk"), QStringLiteral("Desk PC")));
    CHECK(pad.boundId == "mid:desk");
    CHECK(pad.boundLabel == "Desk PC");
    CHECK(padSnapshotOf(slot(QStringLiteral("sdl:1"))).boundId.empty());
}

TEST_CASE("a binding whose host row is gone falls back to the host's id",
          "[diagnostics][recorder]") {
    ControllerSlot orphan = slot(QStringLiteral("sdl:1"));
    orphan.boundConnectionId = QStringLiteral("mid:desk");
    CHECK(padSnapshotOf(orphan).boundLabel == "mid:desk");
    CHECK(boundHostLabelOf(orphan) == QStringLiteral("mid:desk"));
}

TEST_CASE("an unbound slot names no host", "[diagnostics][recorder]") {
    CHECK(boundHostLabelOf(slot(QStringLiteral("sdl:1"))).isEmpty());
}
