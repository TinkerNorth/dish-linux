// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Moonlight host's whole life, end to end, at the coordinator that owns it:
// found or typed in, paired, probed, bound, joined, torn down, forgotten, and
// paired again afterwards. The per-step units live beside this file; what is
// asserted HERE is the sequence, because every defect this suite was written
// for was a step that behaved correctly on its own and left something behind
// for the next one.
//
// TWO RULES CARRY MOST OF IT.
//
// A FORGET LEAVES NOTHING. A host owns nine pieces of state in this client: the
// remembered row (the pairing anchor lives inside it, so the certificate is not
// stored separately and cannot outlive the row), the discovered entry, the
// probe verdict, the app-list cache, the bindings, the controller numbers those
// bindings hold, the live session, the remembered app and the remembered
// controller type. Every one of them is asserted gone below, including the ones
// that only a REPLY STILL IN FLIGHT could put back: probes_ and appCache_ are
// written through QHash::operator[], which inserts, so a late callback is a
// resurrection unless something stops it.
//
// NOTHING FAILS QUIETLY. Every refusal a user can provoke is asserted to leave
// an observable mark, because the live failure this file answers was a Pair
// that produced no visible reaction and no log line, which made it impossible
// to diagnose from the outside.
//
// Nothing here dials a real host. The addresses are TEST-NET-1 (RFC 5737), which
// routes nowhere, and the cases that need an answer stand up a loopback origin
// and talk to that.

#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "core/moonlight/MoonlightSessionUi.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>
#include <memory>
#include <utility>

using namespace dish;
using namespace dish::source::moon;

namespace {

// RFC 5737 TEST-NET-1: guaranteed to route nowhere, so a probe this triggers
// can never reach a machine on the developer's network.
const QString kNowhere = QStringLiteral("192.0.2.1");

// The pairing anchor. Never presented, because nothing here dials with it, so
// its only job is to be non-empty: that is what MoonlightHost::paired() reads.
const QString kAnchor = QStringLiteral("-----BEGIN CERTIFICATE-----\n"
                                       "not-a-real-cert\n"
                                       "-----END CERTIFICATE-----\n");

repository::MoonlightHost pairedHost(const QString& uuid = QStringLiteral("host-uuid")) {
    repository::MoonlightHost host;
    host.uuid = uuid;
    host.name = QStringLiteral("Living room PC");
    host.address = kNowhere;
    host.serverCertPem = kAnchor;
    return host;
}

moonlight::SourceCapabilities plainPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    return source;
}

// Catch2 owns no event loop; spin the suite's QCoreApplication until the
// condition holds, with a ceiling so a stall fails the case instead of hanging.
bool spinUntil(const std::function<bool()>& ready, int timeoutMs = 8000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return ready();
}

// A loopback origin that answers /serverinfo the way a GameStream host does.
// The probe path is plaintext by design (PairStatus is the one thing an
// unpaired client can read), so this is the whole of what it needs.
class InfoHost {
  public:
    explicit InfoHost(QByteArray reply) : reply_(std::move(reply)) {
        listening_ = server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] { accept(); });
    }

    bool listening() const { return listening_; }
    int port() const { return static_cast<int>(server_.serverPort()); }
    int requests() const { return requests_; }

  private:
    void accept() {
        QTcpSocket* sock = server_.nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        auto seen = std::make_shared<QByteArray>();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, seen] {
            seen->append(sock->readAll());
            if (!seen->contains("\r\n\r\n")) { return; }
            ++requests_;
            sock->write(reply_);
            sock->flush();
            sock->disconnectFromHost();
        });
    }

    QTcpServer server_;
    QByteArray reply_;
    bool listening_ = false;
    int requests_ = 0;
};

// A /serverinfo body wrapped in the smallest HTTP/1.1 response that frames it.
// The machine identity travels as <uniqueid>, which is the tag a GameStream
// host actually emits; <uuid> is what the client calls the field it lands in.
QByteArray serverInfo(const QString& uuid, int pairStatus) {
    const QByteArray body = QStringLiteral("<?xml version=\"1.0\"?>"
                                           "<root status_code=\"200\">"
                                           "<hostname>Fixture</hostname>"
                                           "<uniqueid>%1</uniqueid>"
                                           "<appversion>7.1.431</appversion>"
                                           "<state>SUNSHINE_SERVER_FREE</state>"
                                           "<PairStatus>%2</PairStatus>"
                                           "<currentgame>0</currentgame>"
                                           "</root>")
                                .arg(uuid)
                                .arg(pairStatus)
                                .toUtf8();
    return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") + QByteArray::number(body.size()) +
           QByteArray("\r\nConnection: close\r\n\r\n") + body;
}

// A remembered host pointed at a loopback fixture instead of TEST-NET-1.
repository::MoonlightHost hostAt(const InfoHost& fixture,
                                 const QString& uuid = QStringLiteral("host-uuid")) {
    auto host = pairedHost(uuid);
    host.address = QStringLiteral("127.0.0.1");
    host.httpPort = fixture.port();
    return host;
}

// Runs one probe to completion. probeFinished is the signal the host screen
// waits on for every row it re-asks on open, so it is also what the assertions
// key off: a probe that never fires it parks that row on Checking forever.
bool probeAndSettle(MoonlightManager& manager, const QString& uuid) {
    bool finished = false;
    const auto token = QObject::connect(&manager, &MoonlightManager::probeFinished,
                                        [&finished](const QString&) { finished = true; });
    manager.probe(uuid);
    const bool settled = spinUntil([&finished] { return finished; });
    QObject::disconnect(token);
    return settled;
}

// Everything a host can leave behind, read back through the public surface.
// Gathered in one place so a new piece of state has one obvious home and every
// case that cares asserts against the same list.
struct Residue {
    bool known = false;
    bool rowListed = false;
    bool persisted = false;
    bool anchorOnFile = false;
    bool rememberedApp = false;
    bool sessionAlive = false;
    bool pairingInFlight = false;
    bool probeRemembered = false;
    bool appsRemembered = false;
    int bindings = 0;
};

Residue residueOf(const MoonlightManager& manager, const repository::MoonlightHostRepository& repo,
                  const QString& uuid) {
    Residue out;
    out.known = manager.knows(uuid);
    for (const auto& row : manager.rows()) {
        if (row.uuid != uuid) { continue; }
        out.rowListed = true;
        out.rememberedApp = !row.lastAppId.isEmpty();
    }
    if (const auto stored = repo.get(uuid)) {
        out.persisted = true;
        out.anchorOnFile = !stored->serverCertPem.isEmpty();
    }
    out.sessionAlive = manager.session(uuid) != nullptr;
    out.pairingInFlight = manager.pairingActive() && manager.pairingHostUuid() == uuid;
    const auto inputs = manager.uiInputs(uuid, QString());
    // probeAttempted and the apps flags are the two records a reply landing
    // after the Forget would re-create, so they are what proves it did not.
    out.probeRemembered = inputs.probeAttempted;
    out.appsRemembered = inputs.appsRead || inputs.appsFailed || inputs.appsInFlight;
    out.bindings = static_cast<int>(manager.boundSlots(uuid).size());
    return out;
}

} // namespace

// ── Arrival ──────────────────────────────────────────────────────────────────

TEST_CASE("a host typed in by address is remembered unpaired, with its ports",
          "[moonlight][lifecycle]") {
    // The discovery fallback: mDNS does not cross every subnet, so the manual
    // path has to reach the same place the found path does.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);

    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);

    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);
    REQUIRE(manager.knows(uuid));
    const auto row = manager.row(uuid);
    REQUIRE(row.has_value());
    CHECK(row->name == QStringLiteral("Den"));
    CHECK(row->address == kNowhere);
    CHECK(row->discovered);
    CHECK_FALSE(row->paired);
    // Not paired is a fact, not a fault: the row states it and offers Pair.
    CHECK(row->trust == moonlight::HostTrust::NotPaired);
    CHECK(row->phase == moonlight::HostPhase::Idle);

    // The ports are persisted on a stub so a later pair() has them without
    // waiting for another sweep.
    repository::MoonlightHostRepository repo(settings);
    const auto stored = repo.get(uuid);
    REQUIRE(stored.has_value());
    CHECK(stored->httpPort == 47989);
    CHECK(stored->httpsPort == 47984);
    CHECK(stored->serverCertPem.isEmpty());
}

TEST_CASE("adding the same address twice keeps the pairing already on file",
          "[moonlight][lifecycle]") {
    // A re-add is a re-discovery, and a re-discovery carries no anchor. Letting
    // it write an empty one would unpair a host by typing its address again.
    auto settings = test::makeSharedSettings();
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);
    repository::MoonlightHostRepository repo(settings);
    auto host = pairedHost(uuid);
    host.lastAppId = QStringLiteral("1093255277");
    host.lastAppName = QStringLiteral("Steam Big Picture");
    repo.upsert(host);

    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QString(), 47989, 47984);

    const auto stored = repo.get(uuid);
    REQUIRE(stored.has_value());
    CHECK(stored->serverCertPem == kAnchor);
    CHECK(stored->lastAppId == QStringLiteral("1093255277"));
    CHECK(manager.row(uuid)->paired);
}

// ── Pairing, and every way it can refuse ─────────────────────────────────────

TEST_CASE("pairing a host nobody has heard of refuses out loud", "[moonlight][lifecycle]") {
    // THE LIVE FAILURE THIS FILE ANSWERS. A Pair that returns quietly leaves the
    // PIN sheet on an indeterminate spinner and four empty digit cells with
    // nothing to time it out, which is what "I pressed Pair and nothing
    // happened" looks like from the outside. It has to become a STATE.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("addr:198.51.100.7");

    manager.pair(uuid);

    CHECK_FALSE(manager.pairingActive());
    CHECK(manager.pairingRefused(uuid));
    CHECK(manager.pairingRefusedReason(uuid) == QStringLiteral("unreachable"));
    // And the render contract agrees, so a surface reading it shows the
    // refusal rather than a handshake that is not happening.
    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK(inputs.pairingRefused);
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::PairingRefused);
}

TEST_CASE("a refusal names itself and is scoped to the host it happened on",
          "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    const QString mine = QStringLiteral("addr:198.51.100.7");
    const QString other = QStringLiteral("addr:198.51.100.8");

    manager.pair(mine);
    REQUIRE(manager.pairingRefused(mine));

    // Pairing is one global attempt, but a refusal is about ONE host: a sheet
    // open on a different one must not paint itself failed.
    CHECK_FALSE(manager.pairingRefused(other));
    CHECK(manager.pairingRefusedReason(other).isEmpty());
    CHECK(moonlight::sessionUiState(manager.uiInputs(other, QString())) ==
          moonlight::SessionUiState::Checking);

    // A cancel is the user withdrawing the question, so the refusal goes.
    manager.cancelPairing();
    CHECK_FALSE(manager.pairingRefused(mine));
    CHECK(manager.pairingRefusedReason(mine).isEmpty());
}

TEST_CASE("pairing a known host puts a four digit PIN on screen", "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);

    manager.pair(uuid);

    // The attempt is live: the PIN is minted client side and shown immediately,
    // and phase 1 then blocks on the host until a human types it in.
    CHECK(manager.pairingActive());
    CHECK(manager.pairingHostUuid() == uuid);
    CHECK(manager.pairingPin().size() == 4);
    CHECK_FALSE(manager.pairingRefused(uuid));
    CHECK(manager.row(uuid)->phase == moonlight::HostPhase::Pairing);
    CHECK(moonlight::sessionUiState(manager.uiInputs(uuid, QString())) ==
          moonlight::SessionUiState::PairingPin);

    // Nothing is left dialling TEST-NET-1 once the case ends.
    manager.cancelPairing();
    CHECK_FALSE(manager.pairingActive());
}

// ── Binding ──────────────────────────────────────────────────────────────────

TEST_CASE("binding a paired host creates the session and takes controller zero",
          "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    const auto number = manager.bindController(QStringLiteral("pad-a"), uuid,
                                               moonproto::kControllerTypeAuto, plainPad());

    REQUIRE(number.has_value());
    CHECK(*number == 0);
    CHECK(manager.boundHostFor(QStringLiteral("pad-a")) == uuid);
    CHECK(manager.controllerNumber(QStringLiteral("pad-a")) == 0);
    auto* session = manager.session(uuid);
    REQUIRE(session != nullptr);
    CHECK(session->everStarted());
}

TEST_CASE("binding a host nobody has paired records the intent and starts nothing",
          "[moonlight][lifecycle]") {
    // A binding is a DURABLE INTENT. Pairing is remembered trust verified
    // lazily, so the session is attempted when the pad is used and never when
    // the binding is saved; nothing about the host may refuse the answer.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);

    const auto number = manager.bindController(QStringLiteral("pad-a"), uuid,
                                               moonproto::kControllerTypeAuto, plainPad());

    // No controller number, because there is no session to hold one.
    CHECK_FALSE(number.has_value());
    CHECK(manager.boundHostFor(QStringLiteral("pad-a")) == uuid);
    CHECK(manager.session(uuid) == nullptr);
    CHECK(manager.controllerCount(uuid) == 1);
    // Saving is not blocked, and the section says why it is waiting.
    const auto inputs = manager.uiInputs(uuid, QStringLiteral("pad-a"));
    CHECK_FALSE(moonlight::sessionUiBlocksApply(moonlight::sessionUiState(inputs)));
    // Retiring it is a no-op on a session that never existed, not a crash.
    manager.unbindController(QStringLiteral("pad-a"));
    CHECK(manager.boundHostFor(QStringLiteral("pad-a")).isEmpty());
    CHECK(manager.controllerCount(uuid) == 0);
}

TEST_CASE("a binding with no slot or no host is refused rather than half recorded",
          "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    MoonlightManager manager(settings);

    CHECK_FALSE(manager
                    .bindController(QString(), QStringLiteral("host-uuid"),
                                    moonproto::kControllerTypeAuto, plainPad())
                    .has_value());
    CHECK_FALSE(manager
                    .bindController(QStringLiteral("pad-a"), QString(),
                                    moonproto::kControllerTypeAuto, plainPad())
                    .has_value());
    CHECK(manager.boundHostFor(QStringLiteral("pad-a")).isEmpty());
    CHECK(manager.session(QStringLiteral("host-uuid")) == nullptr);
}

TEST_CASE("bindings do not outlive the process, and the pairing does", "[moonlight][lifecycle]") {
    // Pins the CONTRACT, not an aspiration. Bindings are live routing state in
    // this client for satellites and Moonlight alike, and the slot list is
    // rebuilt from the devices actually present at launch. What has to survive
    // is the TRUST and the picks, because those are what a restart cannot
    // re-derive from the hardware in front of it.
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    const QString uuid = QStringLiteral("host-uuid");

    {
        MoonlightManager first(settings);
        REQUIRE(first
                    .bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                                    plainPad())
                    .has_value());
        first.setLastApp(uuid, QStringLiteral("1093255277"), QStringLiteral("Steam Big Picture"));
        first.setControllerType(uuid, moonproto::kControllerTypePs);
    }

    MoonlightManager second(settings);
    CHECK(second.boundHostFor(QStringLiteral("pad-a")).isEmpty());
    CHECK(second.session(uuid) == nullptr);
    const auto row = second.row(uuid);
    REQUIRE(row.has_value());
    CHECK(row->paired);
    CHECK(row->lastAppId == QStringLiteral("1093255277"));
    CHECK(row->controllerType == moonproto::kControllerTypePs);
}

// ── The session: one per host, reference counted ─────────────────────────────

TEST_CASE("a second binding joins, a fifth is refused, and the last one out closes up",
          "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    for (int i = 0; i < 4; ++i) {
        const auto number = manager.bindController(QStringLiteral("pad-%1").arg(i), uuid,
                                                   moonproto::kControllerTypeAuto, plainPad());
        REQUIRE(number.has_value());
        CHECK(static_cast<int>(*number) == i);
    }
    auto* session = manager.session(uuid);
    REQUIRE(session != nullptr);
    // ONE session object throughout: a second would mean a second /launch, and
    // the host answers that with "an app is already running".
    CHECK(session->controllerCount() == 4);

    // The four pad ceiling is the protocol's, and it is the ONE host state that
    // blocks Apply, because the bind behind it is going to refuse.
    CHECK_FALSE(manager
                    .bindController(QStringLiteral("pad-4"), uuid, moonproto::kControllerTypeAuto,
                                    plainPad())
                    .has_value());
    CHECK(manager.boundHostFor(QStringLiteral("pad-4")).isEmpty());
    CHECK(moonlight::sessionUiState(manager.uiInputs(uuid, QStringLiteral("pad-4"))) ==
          moonlight::SessionUiState::HostFull);

    for (int i = 0; i < 3; ++i) {
        manager.unbindController(QStringLiteral("pad-%1").arg(i));
        CHECK(manager.controllerCount(uuid) == 3 - i);
        // Somebody is still riding it, so the launch is left exactly as it was.
        CHECK_FALSE(moonlight::sessionNeedsStart(session->machineState().phase));
    }
    manager.unbindController(QStringLiteral("pad-3"));
    // Nobody is on it, so the app is handed back rather than stranded.
    CHECK(session->machineState().phase == moonlight::SessionPhase::Idle);
    CHECK(manager.controllerCount(uuid) == 0);
}

// ── Probing a host that answers ──────────────────────────────────────────────

TEST_CASE("a probe that is answered settles the trust the row states", "[moonlight][lifecycle]") {
    InfoHost fixture(serverInfo(QStringLiteral("host-uuid"), /*pairStatus=*/1));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(hostAt(fixture));
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    // Before anybody asks, the honest state is a spinner and not a verdict.
    CHECK(moonlight::sessionUiState(manager.uiInputs(uuid, QString())) ==
          moonlight::SessionUiState::Checking);

    REQUIRE(probeAndSettle(manager, uuid));

    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK(inputs.probeAttempted);
    CHECK(inputs.probeAnswered);
    CHECK(inputs.paired);
    CHECK_FALSE(inputs.identityChanged);
    CHECK(moonlight::hostTrust(inputs) == moonlight::HostTrust::Paired);
    CHECK(manager.row(uuid)->trust == moonlight::HostTrust::Paired);
}

TEST_CASE("a host that answers with a different uuid is a different machine",
          "[moonlight][lifecycle]") {
    // The stored certificate anchors a MACHINE. A box reset or replaced behind
    // the same address anchors nothing, and re-pairing is the only way back.
    InfoHost fixture(serverInfo(QStringLiteral("someone-else"), /*pairStatus=*/0));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(hostAt(fixture));
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(probeAndSettle(manager, uuid));

    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK(inputs.identityChanged);
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::HostReplaced);
    CHECK(moonlight::hostTrust(inputs) == moonlight::HostTrust::NotPaired);
}

TEST_CASE("a host that answers unpaired while we remember one has lost the trust",
          "[moonlight][lifecycle]") {
    // The disagreement the other way round from the live report: the client has
    // an anchor and the host has forgotten us. Answered-and-unpaired is a fact
    // about NOW, and it wins over what is remembered.
    InfoHost fixture(serverInfo(QStringLiteral("host-uuid"), /*pairStatus=*/0));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(hostAt(fixture));
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(probeAndSettle(manager, uuid));

    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK(inputs.probeAnswered);
    CHECK_FALSE(inputs.paired);
    CHECK(inputs.remembered);
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::TrustLost);
}

TEST_CASE("a probe with nowhere to send it still finishes", "[moonlight][lifecycle]") {
    // probeFinished is what the host screen waits on for every row it re-asks
    // on open. A probe that returns without firing it parks that row on
    // Checking with nothing left to move it.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("addr:198.51.100.9");

    QStringList finished;
    QObject::connect(&manager, &MoonlightManager::probeFinished,
                     [&finished](const QString& id) { finished.append(id); });
    manager.probe(uuid);

    REQUIRE(finished.size() == 1);
    CHECK(finished.at(0) == uuid);
    // And it recorded nothing, because nothing was learned.
    CHECK_FALSE(manager.uiInputs(uuid, QString()).probeAttempted);
}

// ── Forget ───────────────────────────────────────────────────────────────────

TEST_CASE("forgetting a host leaves not one piece of it behind", "[moonlight][lifecycle]") {
    // The residue check, against every piece of state a host owns. On the
    // Android client the equivalent Forget emptied the host list and left the
    // pinned certificate on file, and a re-pair then met a pin the user
    // believed they had deleted.
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    manager.setLastApp(uuid, QStringLiteral("1093255277"), QStringLiteral("Steam Big Picture"));
    manager.setControllerType(uuid, moonproto::kControllerTypePs);
    REQUIRE(manager
                .bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                                plainPad())
                .has_value());
    REQUIRE(manager
                .bindController(QStringLiteral("pad-b"), uuid, moonproto::kControllerTypeAuto,
                                plainPad())
                .has_value());
    manager.refreshApps(uuid); // paired-only over TLS, so this records a failed read
    REQUIRE(residueOf(manager, repo, uuid).appsRemembered);

    manager.forget(uuid);

    const Residue after = residueOf(manager, repo, uuid);
    CHECK_FALSE(after.known);
    CHECK_FALSE(after.rowListed);
    CHECK_FALSE(after.persisted);
    // The pairing anchor lives inside the row, so it cannot outlive it. This is
    // the assertion the Android defect would have failed.
    CHECK_FALSE(after.anchorOnFile);
    CHECK_FALSE(after.rememberedApp);
    CHECK_FALSE(after.sessionAlive);
    CHECK_FALSE(after.probeRemembered);
    CHECK_FALSE(after.appsRemembered);
    CHECK(after.bindings == 0);
    CHECK(manager.boundHostFor(QStringLiteral("pad-a")).isEmpty());
    CHECK(manager.boundHostFor(QStringLiteral("pad-b")).isEmpty());
    CHECK(manager.controllerCount(uuid) == 0);
    // The controller numbers went with the session, so the next host to use
    // this uuid starts counting from zero rather than from where we left off.
    CHECK_FALSE(manager.controllerNumber(QStringLiteral("pad-a")).has_value());
    // A forgotten host is a stranger, not a paired one: the render contract
    // reads Checking, which is "nobody has asked yet".
    CHECK(moonlight::sessionUiState(manager.uiInputs(uuid, QString())) ==
          moonlight::SessionUiState::Checking);
}

TEST_CASE("forget clears the answers a probe already brought back", "[moonlight][lifecycle]") {
    // A host forgotten and added again is a STRANGER. Rendering it Paired on
    // the strength of a question asked before it was forgotten is exactly the
    // trust the host screen exists to state honestly.
    InfoHost fixture(serverInfo(QStringLiteral("host-uuid"), /*pairStatus=*/1));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(hostAt(fixture));
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(probeAndSettle(manager, uuid));
    REQUIRE(manager.uiInputs(uuid, QString()).paired);

    manager.forget(uuid);

    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK_FALSE(inputs.probeAttempted);
    CHECK_FALSE(inputs.paired);
    CHECK_FALSE(inputs.remembered);
    CHECK(moonlight::hostTrust(inputs) == moonlight::HostTrust::NotPaired);
}

TEST_CASE("forget cancels a pairing in flight so it cannot write the host back",
          "[moonlight][lifecycle]") {
    // The resurrection path. A pairing that finishes ok upserts the row with
    // the certificate it just verified, and it does not ask whether the host is
    // still wanted. Left running, a Forget would look done and then undo itself:
    // an empty host list with the pairing anchor still on file, which is the
    // shape the Android client was found in.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);

    manager.pair(uuid);
    REQUIRE(manager.pairingActive());
    REQUIRE(manager.pairingHostUuid() == uuid);

    manager.forget(uuid);

    CHECK_FALSE(manager.pairingActive());
    CHECK(manager.pairingPin().isEmpty());
    repository::MoonlightHostRepository repo(settings);
    CHECK_FALSE(repo.get(uuid).has_value());
    CHECK_FALSE(manager.knows(uuid));
}

TEST_CASE("forgetting one host is not felt by its neighbour", "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost(QStringLiteral("goes")));
    repo.upsert(pairedHost(QStringLiteral("stays")));
    MoonlightManager manager(settings);

    REQUIRE(manager
                .bindController(QStringLiteral("pad-a"), QStringLiteral("goes"),
                                moonproto::kControllerTypeAuto, plainPad())
                .has_value());
    REQUIRE(manager
                .bindController(QStringLiteral("pad-b"), QStringLiteral("stays"),
                                moonproto::kControllerTypeAuto, plainPad())
                .has_value());

    manager.forget(QStringLiteral("goes"));

    CHECK(manager.knows(QStringLiteral("stays")));
    CHECK(manager.boundHostFor(QStringLiteral("pad-b")) == QStringLiteral("stays"));
    CHECK(manager.controllerCount(QStringLiteral("stays")) == 1);
    CHECK(manager.session(QStringLiteral("stays")) != nullptr);
    CHECK(repo.get(QStringLiteral("stays")).has_value());
}

TEST_CASE("a reply that outlives the forget is dropped, not written back",
          "[moonlight][lifecycle]") {
    // probes_ is written through QHash::operator[], which INSERTS. A probe
    // answered a moment after a Forget would therefore re-create the record the
    // Forget removed, and the row would render Paired on the strength of a
    // question asked about a host that is gone.
    InfoHost fixture(serverInfo(QStringLiteral("host-uuid"), /*pairStatus=*/1));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(hostAt(fixture));
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    manager.probe(uuid);
    // Forgotten while the request is on the wire, before any reply is read.
    manager.forget(uuid);
    REQUIRE(spinUntil([&fixture] { return fixture.requests() > 0; }));
    // Give the reply its chance to land and be handled. There is no signal to
    // wait on, because the point is that the handler does nothing.
    spinUntil([] { return false; }, 300);

    const Residue after = residueOf(manager, repo, uuid);
    CHECK_FALSE(after.known);
    CHECK_FALSE(after.persisted);
    CHECK_FALSE(after.probeRemembered);
    CHECK(moonlight::sessionUiState(manager.uiInputs(uuid, QString())) ==
          moonlight::SessionUiState::Checking);
}

// ── Recovery ─────────────────────────────────────────────────────────────────

TEST_CASE("re-pairing after a forget starts a fresh attempt, not a silent no-op",
          "[moonlight][lifecycle]") {
    // The live sequence, in order: pair, forget, pair again. A Forget drops the
    // discovered entry along with the remembered row, so the id a surface is
    // still holding names nothing, and the second Pair must SAY that rather
    // than do nothing. Finding the host again is what makes it pairable.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);

    manager.pair(uuid);
    REQUIRE(manager.pairingActive());
    manager.forget(uuid);

    manager.pair(uuid);
    CHECK_FALSE(manager.pairingActive());
    CHECK(manager.pairingRefused(uuid));
    CHECK(manager.pairingRefusedReason(uuid) == QStringLiteral("unreachable"));

    // Add it again the way a sweep or the address sheet would, and pairing is
    // available once more. Nothing left over blocks it.
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    manager.pair(uuid);
    CHECK(manager.pairingActive());
    CHECK(manager.pairingPin().size() == 4);
    CHECK_FALSE(manager.pairingRefused(uuid));
    manager.cancelPairing();
}

TEST_CASE("a host forgotten and found again pairs from a clean slate", "[moonlight][lifecycle]") {
    // The whole loop: paired, bound, forgotten, re-added, probed. The probe
    // reports the host unpaired because the anchor went with the row, which is
    // the client and the host agreeing again rather than disagreeing.
    InfoHost fixture(serverInfo(QStringLiteral("host-uuid"), /*pairStatus=*/0));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(hostAt(fixture));
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(manager
                .bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                                plainPad())
                .has_value());
    manager.forget(uuid);

    // Found again at the same address, and remembered under the synthetic id a
    // sweep would give it until serverinfo hands back the real one.
    manager.addManualHost(QStringLiteral("127.0.0.1"), QStringLiteral("Den"), fixture.port(),
                          47984);
    const QString found = QStringLiteral("addr:127.0.0.1");
    REQUIRE(probeAndSettle(manager, found));

    const auto inputs = manager.uiInputs(found, QString());
    CHECK(inputs.probeAnswered);
    CHECK_FALSE(inputs.paired);
    CHECK_FALSE(inputs.remembered);
    // Not paired, not trust lost: nothing is remembered to have lost.
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::NotPaired);
    // And a synthetic id never reads as a replaced machine, because there was
    // never a real uuid to compare the answer against.
    CHECK_FALSE(inputs.identityChanged);
}

TEST_CASE("a host that trusts us while we hold no certificate is not paired",
          "[moonlight][lifecycle]") {
    // THE DISAGREEMENT THE LIVE REPORT LANDED IN, from the other side. A host
    // reports PairStatus against the uniqueid on the request, and this install
    // keeps its identity across a Forget, so a box we forgot still answers 1.
    // That is the host's half of the trust and not ours: the certificate every
    // paired-only call pins against went with the row. Reporting Paired here
    // would hide the Pair button behind a chip that nothing can act on.
    InfoHost fixture(serverInfo(QStringLiteral("host-uuid"), /*pairStatus=*/1));
    REQUIRE(fixture.listening());

    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    auto forgotten = hostAt(fixture);
    forgotten.serverCertPem.clear(); // remembered as a destination, never paired
    repo.upsert(forgotten);
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(probeAndSettle(manager, uuid));

    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK(inputs.probeAnswered);
    CHECK(inputs.paired);           // the host's half
    CHECK_FALSE(inputs.remembered); // ours
    CHECK(moonlight::hostTrust(inputs) == moonlight::HostTrust::NotPaired);
    // Not TrustLost: nothing was lost, and the recovery is an ordinary pairing.
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::NotPaired);
    CHECK(manager.row(uuid)->trust == moonlight::HostTrust::NotPaired);
}

TEST_CASE("choosing a host as a destination writes it down", "[moonlight][lifecycle]") {
    // INTEREST IS DURABLE. A host that exists only in a scan result cannot
    // carry a binding: the next sweep owns that set, and the app pick and the
    // controller type the binding flow writes have nowhere to live. Acting on
    // a host promotes it, unpaired, to a record.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);

    repository::MoonlightHostRepository repo(settings);
    manager.bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                           plainPad());

    const auto stored = repo.get(uuid);
    REQUIRE(stored.has_value());
    CHECK(stored->address == kNowhere);
    // Written as INTEREST, never as trust: the anchor is still only ever
    // produced by a pairing handshake that verified it.
    CHECK(stored->serverCertPem.isEmpty());
    CHECK(manager.row(uuid)->trust == moonlight::HostTrust::NotPaired);

    // And now the pick has somewhere to go, which it did not before.
    manager.setLastApp(uuid, QStringLiteral("1093255277"), QStringLiteral("Steam Big Picture"));
    CHECK(repo.get(uuid)->lastAppId == QStringLiteral("1093255277"));
}

TEST_CASE("remembering a destination never overwrites the pairing on file",
          "[moonlight][lifecycle]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(manager
                .bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                                plainPad())
                .has_value());

    CHECK(repo.get(uuid)->serverCertPem == kAnchor);
}

TEST_CASE("re-binding a slot that already holds a number is a restart, not a second pad",
          "[moonlight][lifecycle]") {
    // What Reconnect after a drop does. A second attach for the same slot would
    // be skipped by the host anyway, and losing the binding would be worse.
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(manager.bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                                   plainPad()) == 0);
    CHECK(manager.bindController(QStringLiteral("pad-a"), uuid, moonproto::kControllerTypeAuto,
                                 plainPad()) == 0);
    CHECK(manager.controllerCount(uuid) == 1);
}

TEST_CASE("an app pick with no host to keep it is refused, not swallowed",
          "[moonlight][lifecycle]") {
    // Only a REMEMBERED host has somewhere to keep a pick. Dropping it quietly
    // is how a choice silently fails to stick, which is unarguable from the
    // user's side and invisible from ours.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("addr:198.51.100.9");

    manager.setLastApp(uuid, QStringLiteral("1093255277"), QStringLiteral("Steam"));
    manager.setControllerType(uuid, moonproto::kControllerTypePs);

    repository::MoonlightHostRepository repo(settings);
    CHECK_FALSE(repo.get(uuid).has_value());
    CHECK_FALSE(manager.row(uuid).has_value());
}

TEST_CASE("quitting the app on a host nobody paired reports the refusal",
          "[moonlight][lifecycle]") {
    // /cancel is HTTPS and paired-only, so there is nothing to send. The caller
    // still has to hear an answer.
    auto settings = test::makeSharedSettings();
    MoonlightManager manager(settings);
    manager.addManualHost(kNowhere, QStringLiteral("Den"), 47989, 47984);
    const QString uuid = QStringLiteral("addr:%1").arg(kNowhere);

    bool sawResult = false;
    bool ok = true;
    QObject::connect(&manager, &MoonlightManager::hostAppCancelled,
                     [&](const QString& id, bool result) {
                         sawResult = id == uuid;
                         ok = result;
                     });
    manager.quitHostApp(uuid);

    CHECK(sawResult);
    CHECK_FALSE(ok);
}
