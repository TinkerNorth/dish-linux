// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A pad's Motion switch decides whether its motion reaches a Moonlight host, as dish-android's
// does: switched off, no sample goes out even while the host asks for them. The switch acts on the
// samples and never on the pad the host holds, so the arrival still declares the pad's sensors and
// turning motion back on costs no replug. The cases run the real manager, session and control
// stream against the fixture host, which asks a PlayStation pad for its motion the way Wolf does.
//
// The host is tests/MoonlightFakeHost.h, on loopback.

#include "MoonlightFakeHost.h"
#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QSslSocket>
#include <QString>

#include <memory>

using namespace dish;
using namespace dish::source::moon;
using dish::test::FakeMoonlightHost;
using dish::test::settle;
using dish::test::spinFor;

namespace {

QString hostId() { return QStringLiteral("host-uuid"); }
QString slotId() { return QStringLiteral("pad-a"); }

repository::MoonlightHost recordFor(const FakeMoonlightHost& host) {
    repository::MoonlightHost stored;
    stored.uuid = hostId();
    stored.name = QStringLiteral("Living room PC");
    stored.address = QStringLiteral("127.0.0.1");
    stored.httpPort = host.httpPort();
    stored.httpsPort = host.httpsPort();
    stored.serverCertPem = host.certPem();
    return stored;
}

// A pad with every sensor and actuator.
moonlight::SourceCapabilities fullPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    source.touchpad = true;
    source.lightbar = true;
    source.motion = true;
    source.battery = true;
    return source;
}

int packetsOfTypeAfter(const FakeMoonlightHost& host, std::size_t seen, std::uint32_t inputType) {
    int count = 0;
    const auto packets = host.controlPackets();
    for (std::size_t i = seen; i < packets.size(); ++i) {
        if (packets[i].inputType == inputType) { ++count; }
    }
    return count;
}

int motionPacketsAfter(const FakeMoonlightHost& host, std::size_t seen) {
    return packetsOfTypeAfter(host, seen, moonproto::kInputControllerMotion);
}

// An arrival or a CONTROLLER_MULTI: what a replug is made of.
int padPacketsAfter(const FakeMoonlightHost& host, std::size_t seen) {
    return packetsOfTypeAfter(host, seen, moonproto::kInputControllerArrival) +
           packetsOfTypeAfter(host, seen, moonproto::kInputControllerMulti);
}

// A listening host, a store holding its record, and a manager over that store whose Motion switch
// answers `motionOn` for every slot.
struct Rig {
    std::shared_ptr<QSettings> settings = test::makeSharedSettings();
    FakeMoonlightHost host;
    std::unique_ptr<repository::MoonlightHostRepository> repo;
    std::unique_ptr<MoonlightManager> manager;
    bool motionOn = true;

    Rig() {
        host.uniqueId = hostId();
        test::seedClientIdentity(*settings);
        repo = std::make_unique<repository::MoonlightHostRepository>(settings);
        repo->upsert(recordFor(host));
        manager = std::make_unique<MoonlightManager>(settings);
        manager->setMotionSwitch([this](const QString&) { return motionOn; });
    }

    // What the binding flow's Apply ends in, as a PlayStation pad.
    std::optional<std::uint8_t> bind() const {
        return manager->bindController(slotId(), hostId(), moonproto::kControllerTypePs, fullPad());
    }

    MoonlightSession* session() const { return manager->session(hostId()); }

    // The pad bound and its session streaming, with the host holding the pad and asking it for
    // both sensors.
    bool bindLive() {
        if (!host.listening() || bind() != 0) { return false; }
        return spinFor([this] {
            auto* live = session();
            return live != nullptr &&
                   live->machineState().phase == moonlight::SessionPhase::Streaming &&
                   live->motionRequested(0, moonproto::kMotionGyroscope) &&
                   live->motionRequested(0, moonproto::kMotionAcceleration);
        });
    }

    // One reading of both sensors, the way the sensor thread hands one over.
    void sendMotionSample() const {
        session()->sendMotion(0, moonproto::kMotionGyroscope, 1.0F, 2.0F, 3.0F);
        session()->sendMotion(0, moonproto::kMotionAcceleration, 4.0F, 5.0F, 6.0F);
    }
};

bool tlsAvailable() { return QSslSocket::supportsSsl(); }

} // namespace

TEST_CASE("a pad whose Motion switch is on sends its motion once the host asks for it",
          "[moonlight][motion]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive());
    const std::size_t before = rig.host.controlPackets().size();

    rig.sendMotionSample();

    CHECK(spinFor([&rig, before] { return motionPacketsAfter(rig.host, before) == 2; }, 5000));
}

TEST_CASE("with no Motion switch set, a pad's motion goes out", "[moonlight][motion]") {
    // A manager nobody handed a switch has no answer of the user's to honour.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    rig.manager->setMotionSwitch(MoonlightManager::MotionSwitch{});
    REQUIRE(rig.bindLive());
    const std::size_t before = rig.host.controlPackets().size();

    rig.sendMotionSample();

    CHECK(spinFor([&rig, before] { return motionPacketsAfter(rig.host, before) == 2; }, 5000));
}

TEST_CASE("a pad whose Motion switch is off sends no motion while the host asks for it",
          "[moonlight][motion]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    rig.motionOn = false;
    REQUIRE(rig.bindLive());
    const std::size_t before = rig.host.controlPackets().size();

    rig.sendMotionSample();

    settle();
    CHECK(motionPacketsAfter(rig.host, before) == 0);
}

TEST_CASE("a pad whose Motion switch is off still arrives with its sensors",
          "[moonlight][motion]") {
    // The switch acts on the samples, as dish-android's does. An arrival without them would make
    // Wolf build another pad for the same number whenever the switch moved, and a replug unplugs
    // the pad in the game.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    rig.motionOn = false;
    REQUIRE(rig.bindLive());

    const auto packets = rig.host.controlPackets();
    REQUIRE_FALSE(packets.empty());
    CHECK(packets.front().inputType == moonproto::kInputControllerArrival);
    CHECK((packets.front().capabilities & moonlight::kCapsReadAtArrival) ==
          moonlight::kCapsReadAtArrival);
    CHECK(rig.host.padType(0) == moonproto::kControllerTypePs);
}

TEST_CASE("turning motion off stops a stream that is already going", "[moonlight][motion]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive());
    rig.sendMotionSample();
    REQUIRE(spinFor([&rig] { return motionPacketsAfter(rig.host, 0) == 2; }, 5000));

    rig.motionOn = false;
    rig.manager->refreshMotionSwitches();
    settle();
    const std::size_t before = rig.host.controlPackets().size();
    rig.sendMotionSample();

    settle();
    CHECK(motionPacketsAfter(rig.host, before) == 0);
}

TEST_CASE("turning motion back on lets the next sample through and replugs nothing",
          "[moonlight][motion]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    rig.motionOn = false;
    REQUIRE(rig.bindLive());
    const std::size_t before = rig.host.controlPackets().size();

    rig.motionOn = true;
    rig.manager->refreshMotionSwitches();
    rig.sendMotionSample();

    CHECK(spinFor([&rig, before] { return motionPacketsAfter(rig.host, before) == 2; }, 5000));
    CHECK(padPacketsAfter(rig.host, before) == 0);
}

TEST_CASE("a slot bound again reads its Motion switch again", "[moonlight][motion]") {
    // Apply writes the switch and then binds, and a slot already on the session keeps its number:
    // the bind is where the new answer has to be read.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive());
    rig.motionOn = false;

    REQUIRE(rig.bind() == 0);
    const std::size_t before = rig.host.controlPackets().size();
    rig.sendMotionSample();

    settle();
    CHECK(motionPacketsAfter(rig.host, before) == 0);
    CHECK(padPacketsAfter(rig.host, before) == 0);
}
