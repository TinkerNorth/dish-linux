// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the client actually SAYS to a host, against a host that answers. The
// sibling files pin the decisions: `shouldHandBackApp` is a pure function,
// `reduce` is a pure reducer, and both are asserted arm by arm. Neither can
// tell whether the decision is wired to a socket, and every live failure this
// work answers was exactly that: a rule that was right and a request that was
// never sent, or sent when it should not have been.
//
// So these cases count requests at the far end. A session is taken all the way
// up here, through a real /launch, a real RTSP handshake and a real ENet
// control link, because the two questions that matter most only exist once it
// is up: does the last controller leaving hand the app back, and is a link that
// went quiet distinguishable from a host that ended the session.
//
// The host is tests/MoonlightFakeHost.h, on loopback. Nothing here reaches a
// machine on the developer's network, and the one case that needs a host which
// never answers uses TEST-NET-1 (RFC 5737), which routes nowhere.

#include "MoonlightFakeHost.h"
#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "core/moonlight/MoonlightSessionUi.h"
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

const QString kHostId = QStringLiteral("host-uuid");

// A remembered, paired record pointing at the fixture, anchored on the
// certificate the fixture actually presents so every TLS call passes the pin.
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

moonlight::SourceCapabilities plainPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    return source;
}

// Everything a case needs, wired together: a listening host, a store holding
// its record, and a manager over that store.
struct Rig {
    std::shared_ptr<QSettings> settings = test::makeSharedSettings();
    FakeMoonlightHost host;
    std::unique_ptr<repository::MoonlightHostRepository> repo;
    std::unique_ptr<MoonlightManager> manager;

    Rig() {
        // The machine behind the address is the one the record anchors: a
        // fixture answering with a different identity is a REPLACED host, and
        // that verdict outranks everything these cases are about.
        host.uniqueId = kHostId;
        test::seedClientIdentity(*settings);
        repo = std::make_unique<repository::MoonlightHostRepository>(settings);
        repo->upsert(recordFor(host));
        manager = std::make_unique<MoonlightManager>(settings);
    }

    // Binds `slotId` and waits for the session to be carrying it on a live
    // control link. False means the session never came up.
    bool bindLive(const QString& slotId) {
        manager->bindController(slotId, kHostId, moonproto::kControllerTypeAuto, plainPad());
        return spinFor([this] {
            auto* session = manager->session(kHostId);
            return session != nullptr &&
                   session->machineState().phase == moonlight::SessionPhase::Streaming;
        });
    }

    // What entering a surface does: trust is remembered and verified lazily,
    // so the binding screen and the hosts screen both re-ask before they
    // render. A verdict read without one is a verdict nobody asked for.
    bool probeHost() {
        bool finished = false;
        const auto token = QObject::connect(manager.get(), &MoonlightManager::probeFinished,
                                            [&finished](const QString&) { finished = true; });
        manager->probe(kHostId);
        const bool settled = spinFor([&finished] { return finished; });
        QObject::disconnect(token);
        return settled;
    }

    moonlight::SessionUiState uiFor(const QString& slotId) const {
        return moonlight::sessionUiState(manager->uiInputs(kHostId, slotId));
    }
};

// The TLS half of the fixture needs a working backend; a Qt built without one
// would fail every case here for a reason that has nothing to do with Dish.
bool tlsAvailable() { return QSslSocket::supportsSsl(); }

} // namespace

// ── A session, end to end ────────────────────────────────────────────────────

TEST_CASE("a first binding launches the app and brings the stream up", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());

    REQUIRE(rig.bindLive(QStringLiteral("pad-a")));

    // The three things the host saw, in order and once each.
    CHECK(rig.host.seen(QStringLiteral("/serverinfo")) == 1);
    CHECK(rig.host.seen(QStringLiteral("/launch")) == 1);
    CHECK(rig.host.seen(QStringLiteral("/resume")) == 0);
    // The pad is announced on the live link, and the media ports have been
    // pinged since SETUP named them.
    REQUIRE(spinFor([&rig] { return rig.host.arrivals() >= 1; }));
    CHECK(rig.host.mediaPings() > 0);

    // And the binding renders as its own place in that session.
    CHECK(rig.manager->session(kHostId)->linkState() == MoonlightLinkState::Live);
    CHECK(rig.uiFor(QStringLiteral("pad-a")) == moonlight::SessionUiState::Live);
    CHECK(rig.manager->controllerNumber(QStringLiteral("pad-a")) == 0);
}

TEST_CASE("a second binding joins the live session without one HTTP call", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a")));

    rig.host.forgetRequests();
    const auto second = rig.manager->bindController(QStringLiteral("pad-b"), kHostId,
                                                    moonproto::kControllerTypeXbox, plainPad());
    REQUIRE(second.has_value());
    REQUIRE(spinFor([&rig] { return rig.host.arrivals() >= 2; }));
    settle();

    // NOT ONE REQUEST. A second /launch is answered "an app is already running"
    // and would take the session down rather than join it.
    CHECK(rig.host.paths().isEmpty());
    CHECK(rig.manager->controllerCount(kHostId) == 2);
    CHECK(*second == 1);
    // A binding that does not exist yet is offered the session, not a picker.
    CHECK(rig.uiFor(QStringLiteral("pad-c")) == moonlight::SessionUiState::Joining);
}

TEST_CASE("two bindings converging in one turn start one session", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());

    // Both in the same turn of the loop, before any reply can have landed:
    // whatever stops the second from launching has to be the code, not timing.
    const auto first = rig.manager->bindController(QStringLiteral("pad-a"), kHostId,
                                                   moonproto::kControllerTypeAuto, plainPad());
    const auto second = rig.manager->bindController(QStringLiteral("pad-b"), kHostId,
                                                    moonproto::kControllerTypeAuto, plainPad());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    REQUIRE(spinFor([&rig] { return rig.host.arrivals() >= 2; }));
    settle();
    CHECK(rig.host.seen(QStringLiteral("/serverinfo")) == 1);
    CHECK(rig.host.seen(QStringLiteral("/launch")) == 1);
    CHECK(rig.manager->controllerCount(kHostId) == 2);
}

// ── Leaving: when a quit is sent, and when it is not ─────────────────────────

TEST_CASE("only the last controller out hands the app back", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a")));
    REQUIRE(rig.manager
                ->bindController(QStringLiteral("pad-b"), kHostId, moonproto::kControllerTypeAuto,
                                 plainPad())
                .has_value());
    REQUIRE(spinFor([&rig] { return rig.host.arrivals() >= 2; }));

    rig.manager->unbindController(QStringLiteral("pad-a"));
    settle();
    // Somebody is still playing. Closing the app on them would be worse than
    // any tidying it saves.
    CHECK(rig.host.seen(QStringLiteral("/cancel")) == 0);
    CHECK(rig.manager->session(kHostId)->machineState().phase ==
          moonlight::SessionPhase::Streaming);
    // The pad that left is unplugged by having its bit cleared while still
    // being named, which is the only unplug the protocol has.
    CHECK(rig.host.lastActiveMask() == 0b10);

    rig.manager->unbindController(QStringLiteral("pad-b"));
    REQUIRE(spinFor([&rig] { return rig.host.seen(QStringLiteral("/cancel")) == 1; }));
    CHECK(rig.manager->session(kHostId)->machineState().phase == moonlight::SessionPhase::Idle);
}

TEST_CASE("a session the host never started is torn down in silence", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    // The host refuses in the BODY of an HTTP 200 and offers no resume, which
    // is a session another device is holding.
    rig.host.launchOk = false;

    rig.manager->bindController(QStringLiteral("pad-a"), kHostId, moonproto::kControllerTypeAuto,
                                plainPad());
    REQUIRE(spinFor([&rig] {
        auto* session = rig.manager->session(kHostId);
        return session != nullptr &&
               session->machineState().phase == moonlight::SessionPhase::Failed;
    }));

    // NOTHING WAS STARTED, SO THERE IS NOTHING TO CANCEL. A /cancel here would
    // close an app belonging to whoever is actually using the host.
    settle();
    CHECK(rig.host.seen(QStringLiteral("/launch")) == 1);
    CHECK(rig.host.seen(QStringLiteral("/cancel")) == 0);
    REQUIRE(rig.probeHost());
    CHECK(rig.uiFor(QStringLiteral("pad-a")) == moonlight::SessionUiState::BusyOther);

    // And the last unbind of that session stays silent too.
    rig.manager->unbindController(QStringLiteral("pad-a"));
    settle();
    CHECK(rig.host.seen(QStringLiteral("/cancel")) == 0);
}

TEST_CASE("a launch that worked and a stream that did not is handed back", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    // The app starts and the handshake behind it does not, which leaves the
    // host running something on our behalf that we cannot use.
    rig.host.rtspAnswers = false;

    rig.manager->bindController(QStringLiteral("pad-a"), kHostId, moonproto::kControllerTypeAuto,
                                plainPad());
    REQUIRE(spinFor([&rig] { return rig.host.seen(QStringLiteral("/cancel")) == 1; }));
    CHECK(rig.host.seen(QStringLiteral("/launch")) == 1);
    REQUIRE(rig.probeHost());
    CHECK(rig.uiFor(QStringLiteral("pad-a")) == moonlight::SessionUiState::SetupFailed);
    // The binding stands: the intent outlives the attempt that failed.
    CHECK(rig.manager->boundHostFor(QStringLiteral("pad-a")) == kHostId);
}

TEST_CASE("a host that ended the session and a link that dropped are not the same thing",
          "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }

    SECTION("the host said so") {
        Rig rig;
        REQUIRE(rig.host.listening());
        REQUIRE(rig.bindLive(QStringLiteral("pad-a")));
        rig.host.forgetRequests();

        rig.host.endSession();
        REQUIRE(spinFor([&rig] {
            return rig.manager->session(kHostId)->machineState().failure ==
                   moonlight::SessionFailure::HostEnded;
        }));

        settle();
        REQUIRE(rig.probeHost());
        CHECK(rig.uiFor(QStringLiteral("pad-a")) == moonlight::SessionUiState::EndedByHost);
        // The app closed on the host, so there is nothing left to cancel.
        CHECK(rig.host.seen(QStringLiteral("/cancel")) == 0);
        // The binding is KEPT, and the next use starts a session rather than
        // resuming one: a fresh /launch, never /resume.
        CHECK(rig.manager->boundHostFor(QStringLiteral("pad-a")) == kHostId);
        rig.host.forgetRequests();
        REQUIRE(rig.bindLive(QStringLiteral("pad-a")));
        CHECK(rig.host.seen(QStringLiteral("/launch")) == 1);
        CHECK(rig.host.seen(QStringLiteral("/resume")) == 0);
    }

    SECTION("the link simply went away") {
        Rig rig;
        REQUIRE(rig.host.listening());
        REQUIRE(rig.bindLive(QStringLiteral("pad-a")));
        rig.host.forgetRequests();

        rig.host.dropLink();
        REQUIRE(spinFor([&rig] {
            return rig.manager->session(kHostId)->machineState().failure ==
                   moonlight::SessionFailure::Dropped;
        }));

        settle();
        // A drop is as likely a blip as an ending, and the host will usually
        // hand the session back. Cancelling would close a running game.
        CHECK(rig.host.seen(QStringLiteral("/cancel")) == 0);
        REQUIRE(rig.probeHost());
        CHECK(rig.uiFor(QStringLiteral("pad-a")) == moonlight::SessionUiState::Dropped);
        CHECK(rig.manager->boundHostFor(QStringLiteral("pad-a")) == kHostId);
    }
}

TEST_CASE("an explicit quit closes the app and then asks the host again", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a")));
    rig.host.forgetRequests();

    bool answered = false;
    QObject::connect(rig.manager.get(), &MoonlightManager::hostAppCancelled,
                     [&answered](const QString&, bool) { answered = true; });
    rig.manager->quitHostApp(kHostId);

    // /cancel answers 200 whether or not anything was running, so believing it
    // proves nothing. Both have to happen: the quit, and the question that
    // learns what it actually did. They travel on different transports, so
    // which lands first is the network's business and not a contract.
    REQUIRE(spinFor([&rig] {
        return rig.host.seen(QStringLiteral("/cancel")) == 1 &&
               rig.host.seen(QStringLiteral("/serverinfo")) >= 1;
    }));
    CHECK(answered);
    // The host is in use by nobody now, whatever still points at it.
    CHECK(rig.manager->row(kHostId)->controllers == 0);
}

TEST_CASE("a forget hands the app back although the record has already gone", "[moonlight][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    Rig rig;
    REQUIRE(rig.host.listening());
    REQUIRE(rig.bindLive(QStringLiteral("pad-a")));
    rig.host.forgetRequests();

    rig.manager->forget(kHostId);

    // THE CANCEL STILL AUTHENTICATES. The session carries its own copy of the
    // host record, certificate and all, so the credentials the quit needs are
    // not the ones the forget destroyed; a session that read the store here
    // would have nothing left to pin against.
    REQUIRE(spinFor([&rig] { return rig.host.seen(QStringLiteral("/cancel")) == 1; }));
    CHECK_FALSE(rig.repo->get(kHostId).has_value());
    CHECK_FALSE(rig.manager->knows(kHostId));
    CHECK(rig.manager->boundHostFor(QStringLiteral("pad-a")).isEmpty());
}

// ── The binding survives whatever the host does ──────────────────────────────

TEST_CASE("a binding to a host that never answers is still a binding", "[moonlight][wire]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repository::MoonlightHost unreachable;
    unreachable.uuid = kHostId;
    unreachable.name = QStringLiteral("Living room PC");
    unreachable.address = QStringLiteral("192.0.2.1"); // TEST-NET-1, routes nowhere
    unreachable.serverCertPem = QStringLiteral("-----BEGIN CERTIFICATE-----\nnot-a-real-cert\n"
                                               "-----END CERTIFICATE-----\n");
    repo.upsert(unreachable);
    MoonlightManager manager(settings);

    REQUIRE(manager
                .bindController(QStringLiteral("pad-a"), kHostId, moonproto::kControllerTypeAuto,
                                plainPad())
                .has_value());
    REQUIRE(spinFor(
        [&manager] {
            auto* session = manager.session(kHostId);
            return session != nullptr &&
                   session->machineState().phase == moonlight::SessionPhase::Failed;
        },
        15000));

    // The attempt failed and the intent did not. Nothing about the host may
    // withdraw what the user asked for.
    CHECK(manager.boundHostFor(QStringLiteral("pad-a")) == kHostId);
    const auto inputs = manager.uiInputs(kHostId, QStringLiteral("pad-a"));
    CHECK_FALSE(moonlight::sessionUiBlocksApply(moonlight::sessionUiState(inputs)));
}
