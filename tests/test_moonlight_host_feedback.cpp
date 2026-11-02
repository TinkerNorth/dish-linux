// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A Moonlight host's feedback is applied on the session's control stream thread, and the stream
// waits for its host holding nothing: a rumble the host sends reaches the manager's sink while the
// Qt loop is not running, and a report sent while the link idles waits for nothing. The host is
// tests/MoonlightFakeHost.h, on loopback.

#include "MoonlightFakeHost.h"
#include "QSettingsFixture.h"
#include "TestEventLoop.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightControlStream.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QSslSocket>
#include <QString>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace dish;
using namespace dish::source::moon;
using namespace std::chrono_literals;
using dish::test::FakeMoonlightHost;
using dish::test::spinFor;

namespace {

const std::array<std::uint8_t, 16> kRikey = {0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xA8,
                                             0xB9, 0xCA, 0xDB, 0xEC, 0xFD, 0x0E, 0x1F, 0x20};

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

moonlight::SourceCapabilities fullPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    source.touchpad = true;
    source.lightbar = true;
    source.motion = true;
    source.battery = true;
    return source;
}

// Waits on the clock alone: the Qt loop does not run, so nothing queued to this thread can land.
bool waitWithoutQt(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// What the manager's rumble sink was handed, and where.
struct Heard {
    std::atomic<int> rumbles{0};
    std::atomic<int> number{-1};
    std::atomic<std::uint32_t> motors{0};
    std::atomic<bool> hostMatched{false};
    std::atomic<bool> onQtThread{false};
};

// A listening host, a store holding its record, and a manager over that store whose rumble sink
// records what reaches it. The sink is installed before any session exists, as AppModel does.
struct Rig {
    std::shared_ptr<QSettings> settings = test::makeSharedSettings();
    FakeMoonlightHost host;
    std::unique_ptr<repository::MoonlightHostRepository> repo;
    std::unique_ptr<MoonlightManager> manager;
    Heard heard;
    const std::thread::id qtThread = std::this_thread::get_id();

    Rig() {
        host.uniqueId = hostId();
        test::seedClientIdentity(*settings);
        repo = std::make_unique<repository::MoonlightHostRepository>(settings);
        repo->upsert(recordFor(host));
        manager = std::make_unique<MoonlightManager>(settings);
        manager->setRumbleSink([this](const std::string& hostUuid, std::uint8_t number,
                                      std::uint16_t strong, std::uint16_t weak) {
            heard.onQtThread.store(std::this_thread::get_id() == qtThread);
            heard.hostMatched.store(hostUuid == hostId().toStdString());
            heard.number.store(number);
            heard.motors.store((static_cast<std::uint32_t>(strong) << 16) | weak);
            heard.rumbles.fetch_add(1);
        });
    }

    // The pad bound as a PlayStation pad and its session streaming.
    bool bindLive() {
        if (!host.listening()) { return false; }
        if (manager->bindController(slotId(), hostId(), moonproto::kControllerTypePs, fullPad()) !=
            0) {
            return false;
        }
        return spinFor([this] {
            auto* live = manager->session(hostId());
            return live != nullptr &&
                   live->machineState().phase == moonlight::SessionPhase::Streaming;
        });
    }
};

bool tlsAvailable() { return QSslSocket::supportsSsl(); }

} // namespace

TEST_CASE("a host's rumble reaches the manager's sink on the control thread, with the Qt loop idle",
          "[moonlight][feedback]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.bindLive());

    rig.host.sendRumble(0, 0x1234, 0x5678);

    REQUIRE(waitWithoutQt([&rig] { return rig.heard.rumbles.load() >= 1; }));
    CHECK(rig.heard.number.load() == 0);
    CHECK(rig.heard.hostMatched.load());
    // The wire's low frequency is the large motor.
    CHECK(rig.heard.motors.load() == ((0x1234U << 16) | 0x5678U));
    CHECK_FALSE(rig.heard.onQtThread.load());
}

TEST_CASE("a report sent while the control stream idles waits for nothing",
          "[moonlight][control-rx]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    FakeMoonlightHost host;
    REQUIRE(host.listening());
    host.useRikey(kRikey);
    MoonlightControlStream stream;
    REQUIRE(stream.start("127.0.0.1", static_cast<std::uint16_t>(host.controlPort()), 0, kRikey));
    REQUIRE(spinFor([&stream] { return stream.isConnected(); }, 5000));
    // The handshake's acknowledgements settle, and the service thread has nothing to do.
    std::this_thread::sleep_for(50ms);

    constexpr int kReports = 64;
    std::vector<double> callUs;
    callUs.reserve(kReports);
    for (int i = 0; i < kReports; ++i) {
        const auto started = std::chrono::steady_clock::now();
        stream.sendControllerMulti(0, 1, 0, 0, 0, static_cast<std::int16_t>(i), 0, 0, 0);
        callUs.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
                .count());
        std::this_thread::sleep_for(5ms);
    }
    std::sort(callUs.begin(), callUs.end());
    CHECK(callUs[callUs.size() / 2] < 500.0);
    CHECK(spinFor(
        [&host] { return host.controlPackets().size() >= static_cast<std::size_t>(kReports); },
        5000));
    stream.stop(false);
}
