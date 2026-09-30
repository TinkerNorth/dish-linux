// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The host's touchpad pick decides whether a pad's touches reach a Moonlight host, as it decides
// what a satellite's descriptor declares: Pad, or a host never picked for, sends them, and Off
// keeps them. The pick is read when the slot binds, which Apply does after writing it. The cases
// run the real manager, session and control stream against the fixture host, which records every
// CONTROLLER_TOUCH it reads.
//
// The host is tests/MoonlightFakeHost.h, on loopback.

#include "MoonlightFakeHost.h"
#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "core/moonlight/MoonlightTouchDiffer.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QSslSocket>
#include <QString>

#include <memory>
#include <optional>
#include <string>

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

// A pad with a touchpad and a gyro.
moonlight::SourceCapabilities touchPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    source.touchpad = true;
    source.lightbar = true;
    source.motion = true;
    source.battery = true;
    return source;
}

int touchesAfter(const FakeMoonlightHost& host, std::size_t seen) {
    int count = 0;
    const auto packets = host.controlPackets();
    for (std::size_t i = seen; i < packets.size(); ++i) {
        if (packets[i].inputType == moonproto::kInputControllerTouch) { ++count; }
    }
    return count;
}

int touchesAfter(const FakeMoonlightHost& host, std::size_t seen, std::uint8_t touchEvent) {
    int count = 0;
    const auto packets = host.controlPackets();
    for (std::size_t i = seen; i < packets.size(); ++i) {
        const bool matches = packets[i].inputType == moonproto::kInputControllerTouch &&
                             packets[i].touchEvent == touchEvent;
        if (matches) { ++count; }
    }
    return count;
}

// A listening host, a store holding its record, and a manager over that store whose host's
// touchpad pick is `pick`.
struct Rig {
    std::shared_ptr<QSettings> settings = test::makeSharedSettings();
    FakeMoonlightHost host;
    std::unique_ptr<repository::MoonlightHostRepository> repo;
    std::unique_ptr<MoonlightManager> manager;
    std::optional<std::string> pick;

    Rig() {
        host.uniqueId = hostId();
        test::seedClientIdentity(*settings);
        repo = std::make_unique<repository::MoonlightHostRepository>(settings);
        repo->upsert(recordFor(host));
        manager = std::make_unique<MoonlightManager>(settings);
        manager->setTouchpadPick([this](const QString&) { return pick; });
    }

    // What the binding flow's Apply ends in.
    std::optional<std::uint8_t> bind(int type) const {
        return manager->bindController(slotId(), hostId(), type, touchPad());
    }

    MoonlightSession* session() const { return manager->session(hostId()); }

    // The pad bound and its session streaming, with the host holding the pad.
    bool bindLive(int type) {
        if (!host.listening() || bind(type) != 0) { return false; }
        return spinFor([this] {
            auto* live = session();
            return live != nullptr &&
                   live->machineState().phase == moonlight::SessionPhase::Streaming &&
                   host.padType(0).has_value();
        });
    }

    // One full-state frame, the way the input thread hands it over: finger 7 down at one spot, or
    // nothing on the pad.
    void fingerDown() const {
        moonlight::TouchFinger finger;
        finger.active = true;
        finger.id = 7;
        finger.x = 0.25F;
        finger.y = 0.5F;
        session()->sendTouchFrame(0, finger, moonlight::TouchFinger{});
    }
    void nothingTouching() const {
        session()->sendTouchFrame(0, moonlight::TouchFinger{}, moonlight::TouchFinger{});
    }
};

bool tlsAvailable() { return QSslSocket::supportsSsl(); }

} // namespace

TEST_CASE("a host never picked for gets a PlayStation pad's touches", "[moonlight][touchpad]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    const std::size_t before = rig.host.controlPackets().size();

    rig.fingerDown();

    CHECK(spinFor([&rig, before] { return touchesAfter(rig.host, before) == 1; }, 5000));
}

TEST_CASE("a host picked Off gets none of the pad's touches", "[moonlight][touchpad]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    rig.pick = "off";
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    const std::size_t before = rig.host.controlPackets().size();

    rig.fingerDown();

    settle();
    CHECK(touchesAfter(rig.host, before) == 0);
}

TEST_CASE("a pad bound as a type with no touchpad sends no touches", "[moonlight][touchpad]") {
    // The Xbox pad a host builds has no touchpad to put them on.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive(moonproto::kControllerTypeXbox));
    const std::size_t before = rig.host.controlPackets().size();

    rig.fingerDown();

    settle();
    CHECK(touchesAfter(rig.host, before) == 0);
}

TEST_CASE("with no touchpad pick to read, a PlayStation pad's touches go out",
          "[moonlight][touchpad]") {
    // A manager nobody handed the store reads every host as never picked for.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    rig.manager->setTouchpadPick(MoonlightManager::TouchpadPick{});
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    const std::size_t before = rig.host.controlPackets().size();

    rig.fingerDown();

    CHECK(spinFor([&rig, before] { return touchesAfter(rig.host, before) == 1; }, 5000));
}

TEST_CASE("a slot bound again reads its host's touchpad pick again", "[moonlight][touchpad]") {
    // Apply writes the pick and then binds, and a slot already on the session keeps its number:
    // the bind is where the new pick has to be read.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    rig.pick = "off";

    REQUIRE(rig.bind(moonproto::kControllerTypePs) == 0);
    const std::size_t before = rig.host.controlPackets().size();
    rig.fingerDown();

    settle();
    CHECK(touchesAfter(rig.host, before) == 0);
}

TEST_CASE("turning the touchpad off lifts the finger the host holds, and on puts it down again",
          "[moonlight][touchpad]") {
    // The host keeps a contact until it is told the finger left, so turning the touches off
    // mid-press must not leave it pressed there.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    rig.fingerDown();
    REQUIRE(spinFor([&rig] { return touchesAfter(rig.host, 0, moonproto::kTouchEventDown) == 1; },
                    5000));

    rig.pick = "off";
    REQUIRE(rig.bind(moonproto::kControllerTypePs) == 0);
    std::size_t before = rig.host.controlPackets().size();
    rig.fingerDown();

    CHECK(spinFor(
        [&rig, before] { return touchesAfter(rig.host, before, moonproto::kTouchEventUp) == 1; },
        5000));
    CHECK(touchesAfter(rig.host, before, moonproto::kTouchEventDown) == 0);

    rig.pick = "ds4";
    REQUIRE(rig.bind(moonproto::kControllerTypePs) == 0);
    before = rig.host.controlPackets().size();
    rig.fingerDown();

    CHECK(spinFor(
        [&rig, before] { return touchesAfter(rig.host, before, moonproto::kTouchEventDown) == 1; },
        5000));
}

TEST_CASE("a finger held through a re-apply is lifted on the host when it lifts",
          "[moonlight][touchpad]") {
    // Applying a binding again keeps the pad under its number, and the host keeps the contact it
    // was told about, so the frame the host was last told about has to survive the bind.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    rig.fingerDown();
    REQUIRE(spinFor([&rig] { return touchesAfter(rig.host, 0, moonproto::kTouchEventDown) == 1; },
                    5000));

    REQUIRE(rig.bind(moonproto::kControllerTypePs) == 0);
    const std::size_t before = rig.host.controlPackets().size();
    rig.nothingTouching();

    CHECK(spinFor(
        [&rig, before] { return touchesAfter(rig.host, before, moonproto::kTouchEventUp) == 1; },
        5000));
}

TEST_CASE("a pad's touch starts from nothing on a stream that comes up again",
          "[moonlight][touchpad]") {
    // The host builds its pads afresh for a new stream, so a finger that stayed down across it is
    // a contact the new pad never saw go down.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    rig.fingerDown();
    REQUIRE(spinFor([&rig] { return touchesAfter(rig.host, 0, moonproto::kTouchEventDown) == 1; },
                    5000));
    rig.host.endSession();
    REQUIRE(spinFor([&rig] {
        return rig.session()->machineState().failure == moonlight::SessionFailure::HostEnded;
    }));

    REQUIRE(rig.bindLive(moonproto::kControllerTypePs));
    const std::size_t before = rig.host.controlPackets().size();
    rig.fingerDown();

    CHECK(spinFor(
        [&rig, before] { return touchesAfter(rig.host, before, moonproto::kTouchEventDown) == 1; },
        5000));
}
