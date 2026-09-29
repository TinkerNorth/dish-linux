// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A pad the host already holds is replugged, never announced twice (host fact H5).
//
// A Moonlight host keeps a controller number it holds and skips a second CONTROLLER_ARRIVAL for
// it, so a binding whose pad changes after it was announced (another emulated type, or motion the
// first arrival did not carry) reaches the host only as an unplug followed by a new arrival under
// the same number. The cases run the real manager, session and control stream against the fixture
// host, which keeps its joypad table by Wolf's rules, so what they assert is the pad the host ends
// up holding.
//
// The host is tests/MoonlightFakeHost.h, on loopback.

#include "MoonlightFakeHost.h"
#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightControlStream.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QSslSocket>
#include <QString>

#include <array>
#include <memory>

using namespace dish;
using namespace dish::source::moon;
using dish::test::FakeMoonlightHost;
using dish::test::settle;
using dish::test::spinFor;

namespace {

const QString kHostId = QStringLiteral("host-uuid");
const std::array<std::uint8_t, 16> kRikey = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                                             0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F};
constexpr std::uint32_t kButtons = moonproto::kStandardButtons;

repository::MoonlightHost recordFor(const FakeMoonlightHost& host) {
    repository::MoonlightHost stored;
    stored.uuid = kHostId;
    stored.name = QStringLiteral("Living room PC");
    stored.address = QStringLiteral("127.0.0.1");
    stored.httpPort = host.httpPort();
    stored.httpsPort = host.httpsPort();
    stored.serverCertPem = host.certPem();
    return stored;
}

moonlight::SourceCapabilities pad(bool motion, bool battery) {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    source.touchpad = true;
    source.lightbar = true;
    source.motion = motion;
    source.battery = battery;
    return source;
}

// The packets that name a pad, which leaves out the keepalive a live stream sends on its own.
int padPacketsAfter(const FakeMoonlightHost& host, std::size_t seen) {
    int count = 0;
    const auto packets = host.controlPackets();
    for (std::size_t i = seen; i < packets.size(); ++i) {
        const bool namesAPad = packets[i].inputType == moonproto::kInputControllerArrival ||
                               packets[i].inputType == moonproto::kInputControllerMulti;
        if (namesAPad) { ++count; }
    }
    return count;
}

// A listening host, a store holding its record, and a manager over that store.
struct Rig {
    std::shared_ptr<QSettings> settings = test::makeSharedSettings();
    FakeMoonlightHost host;
    std::unique_ptr<repository::MoonlightHostRepository> repo;
    std::unique_ptr<MoonlightManager> manager;

    Rig() {
        host.uniqueId = kHostId;
        test::seedClientIdentity(*settings);
        repo = std::make_unique<repository::MoonlightHostRepository>(settings);
        repo->upsert(recordFor(host));
        manager = std::make_unique<MoonlightManager>(settings);
    }

    std::optional<std::uint8_t> bind(const QString& slotId, int type,
                                     const moonlight::SourceCapabilities& source) const {
        return manager->bindController(slotId, kHostId, type, source);
    }

    MoonlightSession* session() const { return manager->session(kHostId); }

    bool streaming() const {
        return session() != nullptr &&
               session()->machineState().phase == moonlight::SessionPhase::Streaming;
    }

    // Binds `slotId` and waits for the session to carry it on a live control link.
    bool bindLive(const QString& slotId, int type, const moonlight::SourceCapabilities& source) {
        bind(slotId, type, source);
        return spinFor([this] { return streaming(); });
    }
};

bool tlsAvailable() { return QSslSocket::supportsSsl(); }

// A control stream of the case's own, keyed the way a /launch would have keyed it.
bool connectDirect(FakeMoonlightHost& host, MoonlightControlStream& stream) {
    host.useRikey(kRikey);
    if (!stream.start("127.0.0.1", static_cast<std::uint16_t>(host.controlPort()), 0, kRikey)) {
        return false;
    }
    return spinFor([&stream] { return stream.isConnected(); }, 5000);
}

} // namespace

TEST_CASE("a second arrival for a number the host holds is skipped", "[moonlight][replug][h5]") {
    // What makes a replug necessary at all, against the real link: announcing a number again is
    // not a way to change the pad under it.
    FakeMoonlightHost host;
    REQUIRE(host.listening());
    MoonlightControlStream stream;
    REQUIRE(connectDirect(host, stream));

    stream.sendControllerArrival(0, moonproto::kControllerTypeXbox, 0x03, kButtons);
    stream.sendControllerArrival(0, moonproto::kControllerTypePs, 0x3B, kButtons);

    REQUIRE(spinFor([&host] { return padPacketsAfter(host, 0) == 2; }, 5000));
    CHECK(host.padType(0) == moonproto::kControllerTypeXbox);
}

TEST_CASE("a replug unplugs the number and plugs the new pad in, back to back",
          "[moonlight][replug][h5]") {
    FakeMoonlightHost host;
    REQUIRE(host.listening());
    MoonlightControlStream stream;
    REQUIRE(connectDirect(host, stream));
    stream.sendControllerArrival(0, moonproto::kControllerTypeXbox, 0x03, kButtons);
    stream.sendControllerArrival(1, moonproto::kControllerTypeXbox, 0x03, kButtons);
    REQUIRE(spinFor([&host] { return padPacketsAfter(host, 0) == 2; }, 5000));

    stream.sendControllerReplug(0, /*otherPadsMask=*/0b10, moonproto::kControllerTypePs, 0x3B,
                                kButtons);

    REQUIRE(spinFor([&host] { return host.padType(0) == moonproto::kControllerTypePs; }, 5000));
    // The pad beside it was never named, so it is exactly where it was.
    CHECK(host.padType(1) == moonproto::kControllerTypeXbox);
    const auto packets = host.controlPackets();
    REQUIRE(packets.size() == 4);
    const auto& unplug = packets[2];
    const auto& arrival = packets[3];
    CHECK(unplug.inputType == moonproto::kInputControllerMulti);
    CHECK(unplug.number == 0);
    CHECK(unplug.mask == 0b10);
    CHECK(arrival.inputType == moonproto::kInputControllerArrival);
    CHECK(arrival.number == 0);
    // Consecutive sequence numbers: nothing was sealed between the two, so no input frame could
    // have plugged a default pad into the gap.
    CHECK(arrival.seq == unplug.seq + 1);
}

TEST_CASE("a live pad that changes type is replugged under its number and nothing closes",
          "[moonlight][replug][h5]") {
    // The binding flow's Apply on a pad that is already streaming.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a"), moonproto::kControllerTypeXbox, pad(true, true)));
    REQUIRE(spinFor([&rig] { return rig.host.padType(0) == moonproto::kControllerTypeXbox; }));
    rig.host.forgetRequests();

    REQUIRE(rig.bind(QStringLiteral("pad-a"), moonproto::kControllerTypePs, pad(true, true)) == 0);

    CHECK(spinFor([&rig] { return rig.host.padType(0) == moonproto::kControllerTypePs; }, 5000));
    CHECK(rig.manager->controllerNumber(QStringLiteral("pad-a")) == 0);
    CHECK(rig.streaming());
    CHECK(rig.host.seen(QStringLiteral("/cancel")) == 0);
    CHECK(rig.host.seen(QStringLiteral("/launch")) == 0);
}

TEST_CASE("a pad that changes type before the stream is up arrives as the new pad",
          "[moonlight][replug][h5]") {
    // Nothing is on the wire yet, so there is nothing to unplug: the declaration changes and the
    // stream announces it when it comes up, on the one launch the session was already making.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bind(QStringLiteral("pad-a"), moonproto::kControllerTypeXbox, pad(false, false)) ==
            0);

    REQUIRE(rig.bind(QStringLiteral("pad-a"), moonproto::kControllerTypePs, pad(false, false)) ==
            0);

    REQUIRE(spinFor([&rig] { return rig.streaming(); }));
    CHECK(spinFor([&rig] { return rig.host.padType(0) == moonproto::kControllerTypePs; }, 5000));
    CHECK(rig.host.seen(QStringLiteral("/launch")) == 1);
}

TEST_CASE("a re-bind that changes nothing the host reads leaves the pad as it is",
          "[moonlight][replug][h5]") {
    // A replug unplugs the pad in the game, so a change the host never reads at arrival is not
    // worth one: here the battery bit, which only decides whether charge reports are sent.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a"), moonproto::kControllerTypePs, pad(true, false)));
    // The arrival and the neutral state behind it.
    REQUIRE(spinFor([&rig] { return padPacketsAfter(rig.host, 0) == 2; }, 5000));
    const std::size_t before = rig.host.controlPackets().size();
    rig.host.forgetRequests();

    REQUIRE(rig.bind(QStringLiteral("pad-a"), moonproto::kControllerTypePs, pad(true, true)) == 0);

    settle();
    CHECK(padPacketsAfter(rig.host, before) == 0);
    CHECK(rig.host.padType(0) == moonproto::kControllerTypePs);
    CHECK(rig.streaming());
    CHECK(rig.host.paths().isEmpty());
}

TEST_CASE("replugging one pad leaves the pads beside it where they are",
          "[moonlight][replug][h5]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(
        rig.bindLive(QStringLiteral("pad-a"), moonproto::kControllerTypeXbox, pad(false, false)));
    REQUIRE(rig.bind(QStringLiteral("pad-b"), moonproto::kControllerTypeXbox, pad(false, false)) ==
            1);
    REQUIRE(spinFor([&rig] { return rig.host.padType(1) == moonproto::kControllerTypeXbox; }));
    const std::size_t before = rig.host.controlPackets().size();
    rig.host.forgetRequests();

    REQUIRE(rig.bind(QStringLiteral("pad-b"), moonproto::kControllerTypePs, pad(false, false)) ==
            1);

    REQUIRE(spinFor([&rig] { return rig.host.padType(1) == moonproto::kControllerTypePs; }, 5000));
    CHECK(rig.host.padType(0) == moonproto::kControllerTypeXbox);
    CHECK(rig.manager->controllerNumber(QStringLiteral("pad-a")) == 0);
    const auto packets = rig.host.controlPackets();
    for (std::size_t i = before; i < packets.size(); ++i) {
        const bool unplugsPadZero = packets[i].inputType == moonproto::kInputControllerMulti &&
                                    packets[i].number == 0 && (packets[i].mask & 0b01) == 0;
        CHECK_FALSE(unplugsPadZero);
    }
    CHECK(rig.host.paths().isEmpty());
}

TEST_CASE("a replugged pad streams motion only once the pad the host built asks for it",
          "[moonlight][replug][h5]") {
    // The host asked the OLD pad for motion. The pad it builds in its place starts with no
    // subscription, and Wolf asks an Xbox pad for none at all.
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a"), moonproto::kControllerTypePs, pad(true, false)));
    REQUIRE(
        spinFor([&rig] { return rig.session()->motionRequested(0, moonproto::kMotionGyroscope); }));

    REQUIRE(rig.bind(QStringLiteral("pad-a"), moonproto::kControllerTypeXbox, pad(true, false)) ==
            0);
    REQUIRE(spinFor([&rig] { return rig.host.padType(0) == moonproto::kControllerTypeXbox; }));

    settle();
    CHECK_FALSE(rig.session()->motionRequested(0, moonproto::kMotionGyroscope));
    CHECK_FALSE(rig.session()->motionRequested(0, moonproto::kMotionAcceleration));
}
