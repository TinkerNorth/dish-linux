// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/SatelliteClient.h"
#include "Network/WifiConnection.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QString>

#include <memory>

using dish::models::ControllerApplyDto;
using dish::models::DiscoveredServer;
using dish::models::SessionViewControllerDto;
using dish::models::SessionViewDto;
using dish::net::SatelliteClient;
using dish::net::SessionState;
using dish::net::WifiConnection;
namespace proto = dish::proto;

namespace {

DiscoveredServer server(const QString& ip = QStringLiteral("10.0.0.1")) {
    DiscoveredServer s;
    s.machineId = QStringLiteral("m1");
    s.ip = ip;
    s.name = QStringLiteral("Pc");
    s.udpPort = 9876;
    s.pairPort = 9443;
    s.httpPort = 9443;
    return s;
}

WifiConnection makeConn() { return WifiConnection(server().id(), server()); }

ControllerApplyDto apply(int idx, std::uint8_t code, int appliedType = proto::kControllerTypeXbox) {
    ControllerApplyDto a;
    a.ctrlIdx = idx;
    a.resultCode = code;
    a.appliedType = appliedType;
    a.result = QString::fromUtf8(proto::applyResultName(code).data(),
                                 static_cast<int>(proto::applyResultName(code).size()));
    return a;
}

SessionViewControllerDto view(int idx, int appliedType, bool active = true) {
    SessionViewControllerDto c;
    c.ctrlIdx = idx;
    c.appliedType = appliedType;
    c.active = active;
    return c;
}

// QSignalSpy needs Qt6::Test, which this target does not link.
struct SignalCounter {
    int count = 0;
    template <class Sender, class Signal> SignalCounter(Sender* s, Signal sig) {
        QObject::connect(s, sig, [this] { ++count; });
    }
};

} // namespace

TEST_CASE("session: initial state is Idle with no connectionId or slots", "[session]") {
    auto conn = makeConn();
    REQUIRE(conn.state() == SessionState::Idle);
    REQUIRE_FALSE(conn.connectionId().has_value());
    REQUIRE(conn.desiredDescriptors().isEmpty());
    REQUIRE(conn.registeredBitmap() == 0);
}

TEST_CASE("session: idFor derives the stable id from the machineId", "[session]") {
    REQUIRE(WifiConnection::idFor(server()) == QStringLiteral("mid:m1"));
}

TEST_CASE("session: attachSlot records the full descriptor with no default-type phase",
          "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypePlayStation, false, false,
                    false);
    const auto d = conn.descriptorFor(QStringLiteral("slot-A"));
    REQUIRE(d.has_value());
    REQUIRE(d->type == proto::kControllerTypePlayStation);
    REQUIRE(d->ctrlIdx == 0);
}

TEST_CASE("session: desiredDescriptors folds caps from the slot's hardware flags", "[session]") {
    auto conn = makeConn();
    // Motion, lightbar AND rumble fold per-slot on the analog-trigger base.
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypePlayStation, /*lightbar=*/true,
                    /*motion=*/true, /*rumble=*/true);
    const auto ds = conn.desiredDescriptors();
    REQUIRE(ds.size() == 1);
    const std::uint16_t caps = ds[0].caps;
    REQUIRE((caps & proto::kCapAnalogTriggers) != 0);
    REQUIRE((caps & proto::kCapRumble) != 0);
    REQUIRE((caps & proto::kCapMotion) != 0);
    REQUIRE((caps & proto::kCapLightbar) != 0);
}

TEST_CASE("session: an Xbox-style pad (no motion/lightbar) advertises neither cap", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, true);
    const auto ds = conn.desiredDescriptors();
    REQUIRE(ds.size() == 1);
    REQUIRE((ds[0].caps & proto::kCapMotion) == 0);
    REQUIRE((ds[0].caps & proto::kCapLightbar) == 0);
    // ...but its probed rumble + the always-on analog triggers.
    REQUIRE((ds[0].caps & proto::kCapRumble) != 0);
    REQUIRE((ds[0].caps & proto::kCapAnalogTriggers) != 0);
}

TEST_CASE("session: rumble is per-slot, a Direct claim's descriptor never advertises it",
          "[session]") {
    // Mirrors dish-android #146: a capability shows only where it fires. The
    // The Direct path has no output write path, so its bind passes
    // hasRumble=false and the satellite must not offer a rumble channel.
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    const auto ds = conn.desiredDescriptors();
    REQUIRE(ds.size() == 1);
    REQUIRE((ds[0].caps & proto::kCapRumble) == 0);
    REQUIRE((ds[0].caps & proto::kCapAnalogTriggers) != 0);
}

TEST_CASE("session: re-attach with a changed rumble flag updates the descriptor", "[session]") {
    // A Standard->Direct path switch re-binds the same pad under a new slot id;
    // the same slot re-attaching with rumble dropped must re-fold, so the
    // late-slot-converge diff sees the caps move and resyncs.
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, true);
    REQUIRE((conn.desiredDescriptors()[0].caps & proto::kCapRumble) != 0);
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    REQUIRE((conn.desiredDescriptors()[0].caps & proto::kCapRumble) == 0);
}

TEST_CASE("session: a second slot allocates a fresh controller index, lowest-free", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    conn.attachSlot(QStringLiteral("slot-B"), proto::kControllerTypeXbox, false, false, false);
    REQUIRE(conn.descriptorFor(QStringLiteral("slot-A"))->ctrlIdx == 0);
    REQUIRE(conn.descriptorFor(QStringLiteral("slot-B"))->ctrlIdx == 1);
}

TEST_CASE("session: detaching one slot frees its index for the next attach", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    conn.attachSlot(QStringLiteral("slot-B"), proto::kControllerTypeXbox, false, false, false);
    conn.detachSlot(QStringLiteral("slot-A")); // frees index 0
    conn.attachSlot(QStringLiteral("slot-C"), proto::kControllerTypeXbox, false, false, false);
    REQUIRE(conn.descriptorFor(QStringLiteral("slot-C"))->ctrlIdx == 0);
    REQUIRE(conn.descriptorFor(QStringLiteral("slot-B"))->ctrlIdx == 1);
}

TEST_CASE("session: detachSlot for an unknown slot is a no-op", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    conn.detachSlot(QStringLiteral("ghost"));
    REQUIRE(conn.descriptorFor(QStringLiteral("slot-A")).has_value());
    REQUIRE(conn.desiredDescriptors().size() == 1);
}

TEST_CASE("session: slotIdForIndex maps an index back to its slot", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    conn.attachSlot(QStringLiteral("slot-B"), proto::kControllerTypeXbox, false, false, false);
    REQUIRE(conn.slotIdForIndex(0) == QStringLiteral("slot-A"));
    REQUIRE(conn.slotIdForIndex(1) == QStringLiteral("slot-B"));
    REQUIRE(conn.slotIdForIndex(9).isEmpty());
}

TEST_CASE("session: wantsMouseControl is true iff a slot requests mouse touchpad mode",
          "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    REQUIRE_FALSE(conn.wantsMouseControl());
}

TEST_CASE("session: applyResults flips the registered flag for an ok slot", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    REQUIRE(conn.registeredBitmap() == 0);
    conn.applyResults({apply(0, proto::kApplyOk)});
    REQUIRE(conn.registeredBitmap() == 0x0001);
}

TEST_CASE("session: a failed apply keeps the slot unregistered and surfaces the failure",
          "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    SignalCounter errSpy(&conn, &WifiConnection::errorOccurred);
    conn.applyResults({apply(0, proto::kApplyPluginFailed)});
    REQUIRE(conn.registeredBitmap() == 0);
    REQUIRE(errSpy.count == 1);
}

TEST_CASE("session: replugFailed keeps the slot live (the previous pad keeps streaming)",
          "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    // The previous pad stays in force, so the slot stays registered and its
    // streams keep flowing even though the failure is surfaced.
    SignalCounter errSpy(&conn, &WifiConnection::errorOccurred);
    conn.applyResults({apply(0, proto::kApplyReplugFailed)});
    REQUIRE(conn.registeredBitmap() == 0x0001);
    REQUIRE(errSpy.count == 1);
}

TEST_CASE("session: registeredBitmap sets one bit per registered controller index", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false,
                    false); // idx 0
    conn.attachSlot(QStringLiteral("slot-B"), proto::kControllerTypeXbox, false, false,
                    false); // idx 1
    conn.applyResults({apply(0, proto::kApplyOk), apply(1, proto::kApplyOk)});
    REQUIRE(conn.registeredBitmap() == 0x0003);
}

TEST_CASE("session: matchesAppliedView is true when the applied set equals desired", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    SessionViewDto v;
    v.controllers = {view(0, proto::kControllerTypeXbox)};
    v.mouseControl.granted = false; // matches wantsMouseControl()==false
    REQUIRE(conn.matchesAppliedView(v));
}

TEST_CASE("session: matchesAppliedView is false on a type divergence", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypePlayStation, false, false,
                    false);
    SessionViewDto v;
    v.controllers = {view(0, proto::kControllerTypeXbox)}; // server applied Xbox, we want DS
    REQUIRE_FALSE(conn.matchesAppliedView(v));
}

TEST_CASE("session: matchesAppliedView is false when a desired slot is missing server-side",
          "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    conn.attachSlot(QStringLiteral("slot-B"), proto::kControllerTypeXbox, false, false, false);
    SessionViewDto v;
    v.controllers = {view(0, proto::kControllerTypeXbox)}; // server missing slot 1
    REQUIRE_FALSE(conn.matchesAppliedView(v));
}

TEST_CASE("session: matchesAppliedView ignores an inactive applied slot", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    SessionViewDto v;
    v.controllers = {view(0, proto::kControllerTypeXbox, /*active=*/false)};
    REQUIRE_FALSE(conn.matchesAppliedView(v));
}

TEST_CASE("session: markConnecting moves Idle -> Linking", "[session]") {
    auto conn = makeConn();
    conn.markConnecting();
    REQUIRE(conn.state() == SessionState::Linking);
}

TEST_CASE("session: markConnected from Idle is rejected and leaves state Idle", "[session]") {
    auto conn = makeConn();
    // markConnected only takes effect from Linking; from Idle it must not promote.
    auto client = std::make_shared<SatelliteClient>();
    conn.markConnected(
        client, QStringLiteral("cid"), 1, false, [] {}, [](std::uint8_t) {}, [] {}, [] {});
    REQUIRE(conn.state() == SessionState::Idle);
}

TEST_CASE("session: markDisconnected from Idle is a no-op", "[session]") {
    auto conn = makeConn();
    conn.markDisconnected();
    REQUIRE(conn.state() == SessionState::Idle);
}

TEST_CASE("session: markStale parks the session in Stale (the 'Needs pairing' cue)", "[session]") {
    auto conn = makeConn();
    conn.markConnecting();
    conn.markStale();
    REQUIRE(conn.state() == SessionState::Stale);
}

TEST_CASE("session: updateServer replaces the server snapshot", "[session]") {
    auto conn = makeConn();
    conn.updateServer(server(QStringLiteral("10.0.0.99")));
    REQUIRE(conn.server().ip == QStringLiteral("10.0.0.99"));
}

TEST_CASE("session: adoptEpoch updates the reconcile reference", "[session]") {
    auto conn = makeConn();
    REQUIRE(conn.lastAppliedEpoch() == -1);
    conn.adoptEpoch(7);
    REQUIRE(conn.lastAppliedEpoch() == 7);
}

TEST_CASE("session: re-declaring a slot's type while idle emits no slotChanged", "[session]") {
    auto conn = makeConn();
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
    SignalCounter changedSpy(&conn, &WifiConnection::slotChanged);
    // While idle a re-declare only updates desired state; the next session PUT
    // carries it, so no per-controller sync is requested.
    conn.attachSlot(QStringLiteral("slot-A"), proto::kControllerTypePlayStation, false, false,
                    false);
    REQUIRE(changedSpy.count == 0);
    REQUIRE(conn.descriptorFor(QStringLiteral("slot-A"))->type ==
            proto::kControllerTypePlayStation);
}

TEST_CASE("session: heartbeat cadence is the satellite-confirmed 2000 ms / 5-miss window",
          "[session][heartbeat]") {
    // Mirrors the satellite's HEARTBEAT_INTERVAL_SEC=2 / HEARTBEAT_MISS_MAX=5.
    REQUIRE(SatelliteClient::kHeartbeatIntervalMs == 2000);
    REQUIRE(SatelliteClient::kHeartbeatMissMax == 5);
    REQUIRE(SatelliteClient::kHeartbeatMissNotResponding == 2);
}

TEST_CASE("session: a destroyed connection announces nothing", "[session][teardown]") {
    // ~WifiConnection tears the client down, and that teardown used to emit
    // changed(). A listener that acts on a signal from a dying object calls back
    // into it, and at app exit its own collaborators may already be freed: the
    // manager relays changed() as poolChanged, ConnectionHub::rebuild() reads
    // through AppModel's ConnectionStore, and that store is a MEMBER, destroyed
    // before ~QObject ever gets to the children. Hence: no signal from a
    // destructor, ever.
    int changes = 0;
    int slotChanges = 0;
    int beforeDelete = 0;
    {
        auto* conn = new WifiConnection(server().id(), server());
        QObject::connect(conn, &WifiConnection::changed, [&changes] { ++changes; });
        QObject::connect(conn, &WifiConnection::slotChanged, [&slotChanges] { ++slotChanges; });
        conn->attachSlot(QStringLiteral("slot-A"), proto::kControllerTypeXbox, false, false, false);
        // Anything but Idle, so the destructor's markDisconnected() does real
        // work instead of taking its early return.
        conn->markConnecting();
        REQUIRE(conn->state() == dish::net::SessionState::Linking);
        REQUIRE(changes > 0); // a live object still announces normally
        beforeDelete = changes;
        delete conn;
    }
    CHECK(changes == beforeDelete);
    CHECK(slotChanges == 0);

    // ...and the same transition on a LIVE object still announces, so what the
    // case above pins is the destructor's silence, not a markDisconnected() that
    // stopped emitting.
    WifiConnection live(server().id(), server());
    int liveChanges = 0;
    QObject::connect(&live, &WifiConnection::changed, [&liveChanges] { ++liveChanges; });
    live.markConnecting();
    const int afterConnect = liveChanges;
    live.markDisconnected();
    CHECK(liveChanges == afterConnect + 1);
}
