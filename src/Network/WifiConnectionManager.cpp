// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnectionManager.h"

#include "PairingOutcome.h"
#include "core/net/IpLiterals.h"
#include "source/connection/DiscoveryGateway.h"
#include "source/connection/LANDiscovery.h"
#include "source/connection/MdnsDiscovery.h"
#include "source/http/SatelliteTlsVerifier.h"
#include "Util/Hex.h"
#include "core/reducer/Backoff.h"
#include "core/reducer/CloseNotify.h"
#include "core/reducer/HostAudioVerdict.h"
#include "core/reducer/ProtocolNegotiation.h"
#include "core/reducer/Reconcile.h"
#include "core/reducer/RestOutcome.h"
#include "core/reducer/ReversePairing.h"
#include "core/wire/SessionCrypto.h"

#include <QCoreApplication>
#include <QHostInfo>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QtGlobal>

#include <random>
#include <type_traits>
#include <variant>

namespace dish::net {

namespace {

// QtInfoMsg floor: the per-tick "still <cause>" line is qCDebug and stays off
// unless someone asks for it with QT_LOGGING_RULES="dish.net.debug=true".
// Without the floor, Q_LOGGING_CATEGORY enables debug by default and a
// switched-off satellite is right back to one line per backoff tick.
Q_LOGGING_CATEGORY(lcNet, "dish.net", QtInfoMsg)

// Pins every user-facing string in this file to one .ts <context> entry.

ConnectionEvent makeError(const QString& msg) { return {ConnectionEventKind::Error, {}, msg}; }

ConnectionEvent pairingRequired(const models::DiscoveredServer& s) {
    return {ConnectionEventKind::PairingRequired, s, {}};
}

// Lives here rather than in core/reducer because it is the Qt-to-pure boundary.
std::vector<reducer::DesiredSlot>
descriptorsToDesired(const QList<models::ControllerDescriptor>& descriptors) {
    std::vector<reducer::DesiredSlot> out;
    out.reserve(static_cast<std::size_t>(descriptors.size()));
    for (const auto& d : descriptors) {
        out.push_back({static_cast<std::uint8_t>(d.ctrlIdx), d.type});
    }
    return out;
}

// What the verdict needs to know about one session PUT's reply, the connect's or the rekey's.
reducer::RestReply restReplyOf(const models::SessionResponse& resp, bool pinMismatch) {
    reducer::RestReply rr;
    rr.status = resp.httpStatus;
    rr.bodyParsed = resp.reachable;
    rr.code = resp.code.value_or(QString()).toStdString();
    rr.pinMismatch = pinMismatch;
    rr.failure = resp.failure;
    return rr;
}

QString unreachableMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "Server unreachable — check it's powered on and on the same Wi-Fi.");
}
// One sentence per cause the user can act on. Everything else falls back to the
// generic line: a wrong-but-specific diagnosis is worse than an honest vague one.
const char* failureName(reducer::TransportFailure failure) {
    switch (failure) {
    case reducer::TransportFailure::None:
        return "none";
    case reducer::TransportFailure::Unreachable:
        return "unreachable";
    case reducer::TransportFailure::Refused:
        return "connection refused";
    case reducer::TransportFailure::TimedOut:
        return "timed out";
    case reducer::TransportFailure::Tls:
        return "TLS failure";
    case reducer::TransportFailure::Aborted:
        return "aborted";
    case reducer::TransportFailure::Other:
        return "other";
    }
    return "other";
}

QString unreachableMsgFor(reducer::TransportFailure failure) {
    switch (failure) {
    case reducer::TransportFailure::Refused:
        // The host is up and answering — it just has nothing on that port. Almost
        // always the satellite not being started, which the generic "check it's
        // powered on" line actively misdirects away from.
        return QCoreApplication::translate(
            "dish::net::WifiConnectionManager",
            "That machine is reachable, but no satellite is listening on it. Start "
            "Satellite there, then try again.");
    case reducer::TransportFailure::TimedOut:
        return QCoreApplication::translate(
            "dish::net::WifiConnectionManager",
            "The satellite stopped responding. It may have gone to sleep or left the "
            "network.");
    case reducer::TransportFailure::Tls:
        return QCoreApplication::translate(
            "dish::net::WifiConnectionManager",
            "Could not set up a secure connection to the satellite.");
    case reducer::TransportFailure::None:
    case reducer::TransportFailure::Unreachable:
    case reducer::TransportFailure::Aborted:
    case reducer::TransportFailure::Other:
        break;
    }
    return unreachableMsg();
}

QString linkFailedMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "The satellite accepted, but the controller link would not open. Try again.");
}

QString ipv6Msg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "This satellite's address is IPv6, and a satellite can only be reached over IPv4.");
}

QString rePairMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "This satellite no longer recognizes this device. Re-pair needed.");
}
QString versionMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "This app and the satellite speak different protocol versions.");
}
// The 409 body names the satellite's range, so the message can say which end is
// behind instead of leaving the user to guess. An unusable body falls back to
// the neutral wording above rather than blaming the wrong side.
QString versionMsgFor(reducer::ProtocolVerdict verdict) {
    switch (verdict) {
    case reducer::ProtocolVerdict::UpdateDish:
        return QCoreApplication::translate(
            "dish::net::WifiConnectionManager",
            "This satellite needs a newer version of Dish. Update the app and retry.");
    case reducer::ProtocolVerdict::UpdateSatellite:
        return QCoreApplication::translate(
            "dish::net::WifiConnectionManager",
            "This satellite is too old for this version of Dish. Update the satellite.");
    case reducer::ProtocolVerdict::Settled:
    case reducer::ProtocolVerdict::RetryLower:
    case reducer::ProtocolVerdict::Unusable:
        break;
    }
    return versionMsg();
}
QString identityChangedMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "This satellite's security identity changed. If it was reinstalled, forget it here and "
        "pair again.");
}
QString wrongPinMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "That PIN wasn't accepted. Check the code on the satellite and try again.");
}
QString pairPendingMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "The satellite hasn't confirmed pairing yet. Try again in a moment.");
}
QString reverseDeclinedMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "The satellite declined this device. Pairing was not approved.");
}
QString reverseTimedOutMsg() {
    return QCoreApplication::translate(
        "dish::net::WifiConnectionManager",
        "Timed out waiting for approval on the satellite. Try again.");
}

// The window an operator needs to read the PIN and approve on the satellite. The
// elapsed clock accumulates from these intervals rather than a wall-clock read,
// so the pure decision is driven by integers the manager fully controls.
constexpr int kReversePollIntervalMs = 1000;
constexpr std::int64_t kReverseDeadlineMs = 120'000;

} // namespace

WifiConnectionManager::WifiConnectionManager(ConnectionStore* store, QObject* parent)
    : QObject(parent), store_(store), http_(new HTTPClient(this)) {
    deviceId_ = store_->getOrCreateDeviceId();
    deviceName_ = QHostInfo::localHostName();
    if (deviceName_.isEmpty()) { deviceName_ = QStringLiteral("Linux"); }
    // TOFU on every HTTPS call, pairing included, keyed by host to match the
    // ConnectionStore pin-migration convention: the first pair pins, and every
    // later call must present the pinned cert. The verifier holds the pin store
    // by reference, so the store must outlive this manager.
    http_->setPinVerifier(
        http::pinVerifierOver(store_->facade().pins(),
                              [this](const QString& host) { return pinGuardsAPairingAt(host); }));
}

bool WifiConnectionManager::pinGuardsAPairingAt(const QString& host) const {
    for (auto* conn : connections_) {
        if (conn->server().ip != host) { continue; }
        if (store_->sharedKey(conn->id()).has_value()) { return true; }
    }
    return false;
}

WifiConnectionManager::~WifiConnectionManager() {
    // This loop exists to tear down live sessions, not to announce anything —
    // and by the time it runs there is nobody left who can safely listen.
    //
    // The manager is a QObject CHILD of AppModel, so it is deleted from
    // ~QObject's deleteChildren(), which is after every AppModel member has
    // already been destroyed — ConnectionStore among them. markDisconnected()
    // emits WifiConnection::changed, the manager relays it as poolChanged, and
    // ConnectionHub::rebuild() then reads through the store's freed
    // unique_ptr<RememberedSatelliteRepository>. That was an access violation on
    // every single exit (0xC0000005, crash.dmp written by the handler, so it
    // looked like a clean quit from outside).
    //
    // Blocking the source signal is the fix that does not depend on which
    // collaborator happens to die first. ~WifiConnection blocks for itself too,
    // which is what covers the connections this loop cannot see: forget() takes
    // one out of the map and leaves it on deleteLater, still a child of this
    // manager and still destroyed from the same deleteChildren() pass.
    for (auto* c : connections_) {
        const QSignalBlocker block(c);
        c->markDisconnected();
    }
}

void WifiConnectionManager::startDiscovery() {
    if (scanning_) { return; }
    scanning_ = true;
    emit scanningChanged();
    auto* watcher = new QFutureWatcher<QList<models::DiscoveredServer>>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        discovered_ = watcher->result();
        scanning_ = false;
        // Persist a moved satellite's new IP BEFORE anything else, so the next
        // launch's autoReconnectAll and any in-flight backoff retry (which
        // re-reads store_->remembered()) target the current address. The only
        // other path that writes a fresh IP is a successful session PUT, which
        // cannot happen while the IP is wrong: that is the "must rescan, then
        // reconnect" trap.
        store_->refreshFromDiscovery(discovered_);
        // The same relearn for the in-memory connection.
        for (const auto& server : discovered_) {
            if (auto* conn = connections_.value(server.id(), nullptr)) {
                if (conn->state() == SessionState::Idle || conn->state() == SessionState::Stale) {
                    conn->updateServer(server);
                }
            }
        }
        // So a moved box reconnects on its own once the scan finds it, with no
        // manual Connect.
        autoReconnectAll();
        emit discoveredChanged();
        emit scanningChanged();
        // No "found nothing" event is emitted: an empty discovered_ with
        // scanning_ false IS that state, and the page binds it directly.
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([] {
        auto mdnsFuture = QtConcurrent::run([] { return MdnsDiscovery::discover(); });
        const QList<models::DiscoveredServer> beacon = LANDiscovery::discover();
        const QList<models::DiscoveredServer> mdns = mdnsFuture.result();
        const QList<models::DiscoveredServer> merged =
            DiscoveryGateway::mergeDiscovered(beacon, mdns);
        qInfo("discovery scan: broadcast=%lld mdns=%lld merged=%lld",
              static_cast<long long>(beacon.size()), static_cast<long long>(mdns.size()),
              static_cast<long long>(merged.size()));
        return merged;
    }));
}

void WifiConnectionManager::wireSlotSync(WifiConnection* conn) {
    const QString id = conn->id();
    // Converging via the per-controller routes keeps the session and UDP keys
    // from churning over a toggle.
    QObject::connect(conn, &WifiConnection::slotChanged, this,
                     [this, id](const QString& slotId) { syncSlot(id, slotId); });
    QObject::connect(conn, &WifiConnection::slotRemoved, this,
                     [this, id](int ctrlIdx) { deleteSlot(id, ctrlIdx); });
}

WifiConnection* WifiConnectionManager::ensureConnection(const models::DiscoveredServer& server) {
    const auto id = WifiConnection::idFor(server);
    if (auto* existing = connections_.value(id, nullptr)) { return existing; }
    auto* conn = new WifiConnection(id, server, this);
    connections_.insert(id, conn);
    QObject::connect(conn, &WifiConnection::changed, this, &WifiConnectionManager::poolChanged);
    QObject::connect(conn, &WifiConnection::telemetryChanged, this,
                     &WifiConnectionManager::poolTelemetryChanged);
    QObject::connect(conn, &WifiConnection::errorOccurred, this,
                     [this](const QString& msg) { emit connectionEvent(makeError(msg)); });
    wireSlotSync(conn);
    emit poolChanged();
    return conn;
}

std::optional<WifiConnectionManager::Credentials>
WifiConnectionManager::credentialsFor(const QString& id) const {
    const auto keyHex = store_->sharedKey(id);
    if (!keyHex.has_value() || keyHex->size() != 64) { return std::nullopt; }
    const auto keyBytes = util::fromHex(keyHex->toStdString());
    if (!keyBytes || keyBytes->size() != 32) { return std::nullopt; }
    Credentials creds;
    std::copy_n(keyBytes->begin(), 32, creds.pairingKey.begin());
    creds.proof = QString::fromStdString(
        wire::computeHmacProof(creds.pairingKey.data(), deviceId_.toStdString()));
    return creds;
}

bool WifiConnectionManager::refusesAddress(const models::DiscoveredServer& server,
                                           ConnectIntent intent) {
    const std::string host = server.ip.toStdString();
    // The satellite binds IPv4 only, for REST, the controller link and discovery alike. Both
    // refusals stay quiet for a silent intent, as every background failure does.
    if (isIpv6Address(host)) {
        emitErrorIfUserInitiated(intent, ipv6Msg());
        return true;
    }
    // Satellites are LAN-only by definition, so a public literal here means a
    // spoofed beacon or a poisoned remembered entry. Dialing it would leak the
    // deviceId and hmacProof to an arbitrary internet host.
    if (!isPrivateHostLiteral(host)) {
        emitErrorIfUserInitiated(
            intent, tr("Refusing to connect to a non-local address (%1).").arg(server.ip));
        return true;
    }
    return false;
}

void WifiConnectionManager::clearForUserAction(const QString& id) {
    retryAttempts_.remove(id);
    lastFailure_.remove(id);
    heldByUser_.remove(id);
}

void WifiConnectionManager::connectTo(const models::DiscoveredServer& server,
                                      ConnectIntent intent) {
    if (refusesAddress(server, intent)) { return; }
    const bool isSilent = intent != ConnectIntent::UserInitiated;
    const bool userHeldItDown = heldByUser_.contains(WifiConnection::idFor(server));
    if (isSilent && userHeldItDown) { return; }
    auto* conn = ensureConnection(server);
    if (!isSilent) { clearForUserAction(conn->id()); }
    if (conn->state() == SessionState::Live || conn->state() == SessionState::Linking) {
        conn->updateServer(server);
        return;
    }
    conn->updateServer(server);
    conn->markConnecting();
    // With a key in hand, skip the pair handshake so a moved or offline server
    // fails fast in the session PUT instead of bouncing through PairingRequired
    // and trapping the user behind a PIN prompt.
    if (credentialsFor(conn->id()).has_value()) {
        openSession(conn, server, intent);
    } else {
        pairAndConnect(conn, server, intent);
    }
}

void WifiConnectionManager::pairWithPin(const models::DiscoveredServer& server,
                                        const QString& pin) {
    if (refusesAddress(server, ConnectIntent::UserInitiated)) { return; }
    auto* conn = ensureConnection(server);
    clearForUserAction(conn->id());
    if (conn->state() == SessionState::Live) { return; }
    conn->updateServer(server);
    conn->markConnecting();

    pairingInFlight_.insert(conn->id());
    emit pairingInFlightChanged();
    http_->pair(server.ip, server.pairPort, deviceId_, deviceName_, pin, QString(),
                [this, id = conn->id(), sentOn = QPointer<WifiConnection>(conn),
                 server](const models::PairResponse& response, bool pinMismatch) {
                    onPinPairReply(id, sentOn, server,
                                   PairingOutcome::classify(response, pinMismatch));
                });
}

// Path A: the PIN the operator read off the satellite, typed into the sheet here.
void WifiConnectionManager::onPinPairReply(const QString& id,
                                           const QPointer<WifiConnection>& sentOn,
                                           const models::DiscoveredServer& server,
                                           const PairingOutcome::Arm& outcome) {
    endPairingRequest(id, sentOn);
    // Forgotten while the POST was out, or forgotten and paired afresh: keeping the key would pair
    // a satellite the user removed, or key a newer attempt with an older answer.
    auto* conn = replyTarget(id, sentOn);
    if (conn == nullptr) { return; }
    std::visit(
        [&](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, PairingOutcome::Success>) {
                store_->setSharedKey(arm.sharedKeyHex, id);
                openSession(conn, server, ConnectIntent::UserInitiated);
            } else if constexpr (std::is_same_v<T, PairingOutcome::IdentityChanged>) {
                // No pairingFailed: every reasonToken the sheet knows would
                // blame the PIN or the network. The error channel already
                // clears its submitting state, and it carries the real cue.
                conn->markDisconnected();
                emit connectionEvent(makeError(identityChangedMsg()));
            } else if constexpr (std::is_same_v<T, PairingOutcome::VersionMismatch>) {
                conn->markDisconnected();
                emit connectionEvent(makeError(versionMsg()));
                emit pairingFailed(id, QStringLiteral("versionMismatch"));
            } else if constexpr (std::is_same_v<T, PairingOutcome::AuthRequired>) {
                // Reachable and parsed but no key granted, so the PIN was
                // wrong or expired.
                conn->markDisconnected();
                emit connectionEvent(makeError(wrongPinMsg()));
                emit pairingFailed(id, QStringLiteral("wrongPin"));
            } else if constexpr (std::is_same_v<T, PairingOutcome::Unreachable>) {
                conn->markDisconnected();
                emit connectionEvent(makeError(unreachableMsg()));
                emit pairingFailed(id, QStringLiteral("unreachable"));
            } else {
                // Pending: staged but not granted, rare on a direct submit.
                conn->markDisconnected();
                emit connectionEvent(makeError(pairPendingMsg()));
                emit pairingFailed(id, QStringLiteral("pending"));
            }
        },
        outcome);
}

WifiConnection* WifiConnectionManager::replyTarget(const QString& id,
                                                   const QPointer<WifiConnection>& sentOn) const {
    auto* current = connections_.value(id, nullptr);
    const bool isTheConnectionItWentOutOn = current != nullptr && current == sentOn.data();
    return isTheConnectionItWentOutOn ? current : nullptr;
}

void WifiConnectionManager::endPairingRequest(const QString& id,
                                              const QPointer<WifiConnection>& sentOn) {
    auto* current = connections_.value(id, nullptr);
    const bool aNewerConnectionOwnsTheFlag = current != nullptr && current != sentOn.data();
    if (aNewerConnectionOwnsTheFlag) { return; }
    pairingInFlight_.remove(id);
    emit pairingInFlightChanged();
}

// The value is random but the shape is fixed by the pure formatter, so the displayed PIN is always
// exactly 4 digits. Randomness stays out of the tested decision core.
QString WifiConnectionManager::drawReversePin() {
    std::random_device rd;
    return QString::fromStdString(reducer::formatReversePin(rd()));
}

void WifiConnectionManager::armReverseAttempt(const models::DiscoveredServer& server) {
    reversePin_ = drawReversePin();
    reverseServer_ = server;
    reverseServerName_ = server.name.isEmpty() ? server.ip : server.name;
    reverseElapsedMs_ = 0;
    reverseDeadlineMs_ = kReverseDeadlineMs;
    reverseSawPending_ = false;
    setReversePhase(ReversePairingPhase::AwaitingApproval);
}

// True while this reply still belongs to the attempt that is on screen. A cancel or a restart
// landing while the POST was in flight makes it a late reply for a superseded request, which must
// not start a poll loop of its own.
bool WifiConnectionManager::reverseAttemptIsCurrent(const models::DiscoveredServer& server,
                                                    const QString& pin) const {
    return reversePhase_ == ReversePairingPhase::AwaitingApproval &&
           reverseServer_.id() == server.id() && reversePin_ == pin;
}

// The expected arm: the operator has not answered yet, so the approval poll starts.
void WifiConnectionManager::startReversePoll() {
    if (reverseTimer_ == nullptr) {
        reverseTimer_ = new QTimer(this);
        reverseTimer_->setInterval(kReversePollIntervalMs);
        QObject::connect(reverseTimer_, &QTimer::timeout, this,
                         &WifiConnectionManager::pollReverseStatus);
    }
    reverseTimer_->start();
}

// The operator approved on the satellite, or it granted outright: key the connection and open it.
// Found or made by server rather than carried over, since a round trip lies behind either arm.
void WifiConnectionManager::adoptReverseGrant(const models::DiscoveredServer& server,
                                              const QString& sharedKeyHex) {
    auto* conn = ensureConnection(server);
    conn->markConnecting();
    store_->setSharedKey(sharedKeyHex, WifiConnection::idFor(server));
    if (reverseTimer_ != nullptr) { reverseTimer_->stop(); }
    setReversePhase(ReversePairingPhase::Approved);
    openSession(conn, server, ConnectIntent::UserInitiated);
}

void WifiConnectionManager::applyReverseOutcome(const models::DiscoveredServer& server,
                                                const models::PairResponse& response,
                                                bool pinMismatch) {
    std::visit(
        [&](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, PairingOutcome::Success>) {
                // Approved synchronously, with no operator step.
                adoptReverseGrant(server, arm.sharedKeyHex);
            } else if constexpr (std::is_same_v<T, PairingOutcome::Pending>) {
                startReversePoll();
            } else if constexpr (std::is_same_v<T, PairingOutcome::VersionMismatch>) {
                emit connectionEvent(makeError(versionMsg()));
                finishReverse(ReversePairingPhase::VersionMismatch);
            } else if constexpr (std::is_same_v<T, PairingOutcome::IdentityChanged>) {
                emit connectionEvent(makeError(identityChangedMsg()));
                finishReverse(ReversePairingPhase::IdentityChanged);
            } else if constexpr (std::is_same_v<T, PairingOutcome::Unreachable>) {
                // The transport's own words ("connect failed") are not a sentence for a user.
                emit connectionEvent(makeError(unreachableMsg()));
                finishReverse(ReversePairingPhase::TimedOut);
            } else {
                // AuthRequired: reachable, but no pending grant was staged.
                emit connectionEvent(makeError(response.error.value_or(unreachableMsg())));
                finishReverse(ReversePairingPhase::TimedOut);
            }
        },
        PairingOutcome::classify(response, pinMismatch));
}

void WifiConnectionManager::onReversePairReply(const QString& id,
                                               const QPointer<WifiConnection>& sentOn,
                                               const models::DiscoveredServer& server,
                                               const QString& pin,
                                               const models::PairResponse& response,
                                               bool pinMismatch) {
    endPairingRequest(id, sentOn);
    if (!reverseAttemptIsCurrent(server, pin)) { return; }
    applyReverseOutcome(server, response, pinMismatch);
}

void WifiConnectionManager::requestReversePairing(const models::DiscoveredServer& server) {
    // A fresh request supersedes any in-flight one and clears a previous attempt's terminal arm.
    cancelReversePairing();
    if (refusesAddress(server, ConnectIntent::UserInitiated)) { return; }

    auto* conn = ensureConnection(server);
    clearForUserAction(conn->id());
    conn->updateServer(server);
    armReverseAttempt(server);

    const QString pin = reversePin_;
    // The happy-path reply is {ok:false, pending:true}, which then gets polled.
    pairingInFlight_.insert(conn->id());
    emit pairingInFlightChanged();
    // Empty operator pin, displayed pin as clientPin: that is what selects Path B server-side.
    http_->pair(server.ip, server.pairPort, deviceId_, deviceName_, QString(), pin,
                [this, id = conn->id(), sentOn = QPointer<WifiConnection>(conn), server,
                 pin](const models::PairResponse& response, bool pinMismatch) {
                    onReversePairReply(id, sentOn, server, pin, response, pinMismatch);
                });
}

// What the reducer needs to know about one /pairstatus answer.
reducer::ApprovalReply WifiConnectionManager::approvalReplyOf(const models::PairResponse& status) {
    reducer::ApprovalReply ar;
    ar.status = status.httpStatus;
    ar.bodyParsed = status.reachable;
    ar.statusStr = status.status.value_or(QString()).toStdString();
    ar.hasSharedKey = status.sharedKey.has_value() && !status.sharedKey->isEmpty();
    return ar;
}

// `sharedKeyHex` is what the Approve arm adopts; the reducer only says Approve when the reply
// carried one, and value_or at the call site keeps that invariant local rather than asking a reader
// to carry it across two files.
void WifiConnectionManager::applyReverseAction(reducer::ReversePairingAction action,
                                               const QString& sharedKeyHex,
                                               const models::DiscoveredServer& server) {
    switch (action) {
    case reducer::ReversePairingAction::Approve:
        adoptReverseGrant(server, sharedKeyHex);
        break;
    case reducer::ReversePairingAction::Decline:
        emit connectionEvent(makeError(reverseDeclinedMsg()));
        finishReverse(ReversePairingPhase::Declined);
        break;
    case reducer::ReversePairingAction::TimeOut:
        emit connectionEvent(makeError(reverseTimedOutMsg()));
        finishReverse(ReversePairingPhase::TimedOut);
        break;
    case reducer::ReversePairingAction::KeepPolling:
        break; // the timer re-fires on its own
    }
}

// The poll slot is free again the moment a reply lands, whether or not the reply is still wanted:
// a superseded GET that left the flag set would stall every later poll of the next attempt.
void WifiConnectionManager::onReverseStatusReply(const models::PairResponse& status,
                                                 bool pinMismatch,
                                                 const models::DiscoveredServer& server) {
    reversePollInFlight_ = false;
    // A cancel or restart raced this GET, so its reply is superseded.
    if (reversePhase_ != ReversePairingPhase::AwaitingApproval ||
        reverseServer_.id() != server.id()) {
        return;
    }
    // Terminal, and ahead of the approval ladder: polling on would just spend the operator's whole
    // window against a box we can no longer authenticate.
    if (pinMismatch) {
        emit connectionEvent(makeError(identityChangedMsg()));
        finishReverse(ReversePairingPhase::IdentityChanged);
        return;
    }
    const auto reply = approvalReplyOf(status);
    const auto approval = reducer::classifyApproval(reply, reverseSawPending_);
    // Latched AFTER classifying, so the first pending answer is classified as the first one.
    if (reply.statusStr == "pending") { reverseSawPending_ = true; }
    applyReverseAction(
        reducer::nextReversePairingAction(approval, reverseElapsedMs_, reverseDeadlineMs_),
        status.sharedKey.value_or(QString()), server);
}

void WifiConnectionManager::pollReverseStatus() {
    if (reversePhase_ != ReversePairingPhase::AwaitingApproval) { return; }
    // A slow GET must not stack behind the 1 s timer.
    if (reversePollInFlight_) { return; }
    reversePollInFlight_ = true;
    reverseElapsedMs_ += kReversePollIntervalMs;

    const models::DiscoveredServer server = reverseServer_;
    http_->pairStatus(server.ip, server.pairPort, deviceId_,
                      [this, server](const models::PairResponse& status, bool pinMismatch) {
                          onReverseStatusReply(status, pinMismatch, server);
                      });
}

void WifiConnectionManager::cancelReversePairing() {
    if (reverseTimer_ != nullptr) { reverseTimer_->stop(); }
    reversePollInFlight_ = false;
    if (reversePhase_ != ReversePairingPhase::Idle) {
        reversePin_.clear();
        reverseServerName_.clear();
        reverseServer_ = {};
        reverseElapsedMs_ = 0;
        setReversePhase(ReversePairingPhase::Idle);
    }
}

void WifiConnectionManager::finishReverse(ReversePairingPhase terminal) {
    if (reverseTimer_ != nullptr) { reverseTimer_->stop(); }
    reversePollInFlight_ = false;
    // The pin and server name survive the terminal arm so the sheet can still
    // name what it was pairing; the next request or cancel clears them.
    setReversePhase(terminal);
}

void WifiConnectionManager::setReversePhase(ReversePairingPhase phase) {
    reversePhase_ = phase;
    emit reversePairingChanged();
}

void WifiConnectionManager::pairAndConnect(WifiConnection* conn,
                                           const models::DiscoveredServer& server,
                                           ConnectIntent intent) {
    pairingInFlight_.insert(conn->id());
    emit pairingInFlightChanged();
    http_->pair(server.ip, server.pairPort, deviceId_, deviceName_, QString(), QString(),
                [this, id = conn->id(), sentOn = QPointer<WifiConnection>(conn), server,
                 intent](const models::PairResponse& response, bool pinMismatch) {
                    onConnectPairReply(id, sentOn, server, intent,
                                       PairingOutcome::classify(response, pinMismatch));
                });
}

// The pair a connect starts with when no key is on file, sent without a PIN.
void WifiConnectionManager::onConnectPairReply(const QString& id,
                                               const QPointer<WifiConnection>& sentOn,
                                               const models::DiscoveredServer& server,
                                               ConnectIntent intent,
                                               const PairingOutcome::Arm& outcome) {
    endPairingRequest(id, sentOn);
    auto* conn = replyTarget(id, sentOn);
    if (conn == nullptr) { return; }
    std::visit(
        [&](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, PairingOutcome::Success>) {
                store_->setSharedKey(arm.sharedKeyHex, id);
                openSession(conn, server, intent);
            } else if constexpr (std::is_same_v<T, PairingOutcome::AuthRequired> ||
                                 std::is_same_v<T, PairingOutcome::Pending>) {
                // First-time pair, or the server forgot us. Silent intents
                // park in Stale so the next user tap gets the dialog.
                if (intent == ConnectIntent::UserInitiated) {
                    conn->markDisconnected();
                    emit connectionEvent(pairingRequired(server));
                } else {
                    conn->markStale();
                }
            } else if constexpr (std::is_same_v<T, PairingOutcome::VersionMismatch>) {
                conn->markDisconnected();
                emitErrorIfUserInitiated(intent, versionMsg());
            } else if constexpr (std::is_same_v<T, PairingOutcome::IdentityChanged>) {
                conn->markDisconnected();
                emitErrorIfUserInitiated(intent, identityChangedMsg());
            } else {
                if (intent == ConnectIntent::UserInitiated) {
                    conn->markDisconnected();
                    emit connectionEvent(makeError(unreachableMsg()));
                } else {
                    conn->markStale();
                }
            }
        },
        outcome);
}

void WifiConnectionManager::openSession(WifiConnection* conn,
                                        const models::DiscoveredServer& server,
                                        ConnectIntent intent) {
    const QString id = conn->id();
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) {
        onTerminalAuthFailure(conn, id, intent);
        return;
    }
    const auto descriptors = conn->desiredDescriptors();
    // Lets the reply converge slot changes that raced the round-trip.
    // NOT const: a const capture is copied rather than moved into the
    // std::function, and that copy can throw out of the closure's move ctor.
    auto sentDescriptors = descriptorsToDesired(descriptors);

    http_->putSession(server.ip, server.httpPort, deviceId_, deviceName_, creds->proof, descriptors,
                      conn->wantsMouseControl(), conn->offeredProtocolVersion(),
                      [this, id, sentOn = QPointer<WifiConnection>(conn), server, intent,
                       pairingKey = creds->pairingKey,
                       sentDescriptors](const models::SessionResponse& resp, bool pinMismatch) {
                          onSessionReply(id, sentOn, server, intent, pairingKey, sentDescriptors,
                                         resp, pinMismatch);
                      });
}

std::optional<WifiConnectionManager::SessionMaterial>
WifiConnectionManager::sessionMaterialFrom(const models::SessionResponse& resp,
                                           const std::array<std::uint8_t, 32>& pairingKey) {
    if (!resp.token.has_value() || !resp.sessionSalt.has_value()) { return std::nullopt; }
    const auto tok = util::fromHex(resp.token->toStdString());
    const auto salt = util::fromHex(resp.sessionSalt->toStdString());
    if (!tok || tok->size() != 4 || !salt || salt->size() != wire::kSessionSaltSize) {
        return std::nullopt;
    }
    SessionMaterial m;
    std::copy_n(tok->begin(), 4, m.token.begin());
    const std::uint32_t tokenBe = (static_cast<std::uint32_t>(m.token[0]) << 24) |
                                  (static_cast<std::uint32_t>(m.token[1]) << 16) |
                                  (static_cast<std::uint32_t>(m.token[2]) << 8) |
                                  static_cast<std::uint32_t>(m.token[3]);
    wire::deriveSessionKey(pairingKey.data(), salt->data(), tokenBe, m.sessionKey.data());
    return m;
}

void WifiConnectionManager::onSessionReply(const QString& id,
                                           const QPointer<WifiConnection>& sentOn,
                                           const models::DiscoveredServer& server,
                                           ConnectIntent intent,
                                           const std::array<std::uint8_t, 32>& pairingKey,
                                           const std::vector<reducer::DesiredSlot>& sentDescriptors,
                                           const models::SessionResponse& resp, bool pinMismatch) {
    // Forgotten while the PUT was out, or forgotten and paired afresh: a session started now would
    // bring back what the user removed, or land on a newer attempt it does not belong to.
    auto* conn = replyTarget(id, sentOn);
    if (conn == nullptr) { return; }
    if (conn->state() != SessionState::Linking) {
        handBackLateGrant(id, server, resp);
        return;
    }
    const auto verdict = reducer::classifyRest(restReplyOf(resp, pinMismatch));
    if (verdict != reducer::RestVerdict::Ok || !resp.connectionId || !resp.token ||
        !resp.sessionSalt) {
        onSessionRefused(conn, server, intent, verdict, resp);
        return;
    }
    // Malformed material degrades like a refused connect, never a crash.
    const auto material = sessionMaterialFrom(resp, pairingKey);
    if (!material.has_value()) {
        onGrantUnusable(conn, server, intent, *resp.connectionId);
        return;
    }
    auto client = std::make_shared<SatelliteClient>();
    const bool socketOpened = client->openSocket(server.ip.toStdString(), server.udpPort);
    if (!socketOpened) {
        onGrantUnusable(conn, server, intent, *resp.connectionId);
        return;
    }
    startSession(conn, server, client, *resp.connectionId, resp, *material);
    convergeLateSlots(conn, sentDescriptors);
}

// Handed straight back, or the satellite holds the slot until its own timeout. No retry, even for a
// silent intent: it would be granted the same session and fail the same way.
void WifiConnectionManager::onGrantUnusable(WifiConnection* conn,
                                            const models::DiscoveredServer& server,
                                            ConnectIntent intent, const QString& connectionId) {
    conn->markDisconnected();
    releaseSession(conn->id(), server, connectionId);
    emitErrorIfUserInitiated(intent, linkFailedMsg());
}

// Each refusal has its own way out. An Ok arrives here only when it is missing part of the session.
void WifiConnectionManager::onSessionRefused(WifiConnection* conn,
                                             const models::DiscoveredServer& server,
                                             ConnectIntent intent, reducer::RestVerdict verdict,
                                             const models::SessionResponse& resp) {
    switch (verdict) {
    case reducer::RestVerdict::Unauthorized:
        onTerminalAuthFailure(conn, conn->id(), intent);
        return;
    case reducer::RestVerdict::VersionMismatch:
        onSessionVersionRefused(conn, server, intent, resp);
        return;
    case reducer::RestVerdict::IdentityChanged:
        // The pin still guards the OLD cert, so no retry can succeed: it
        // takes a Forget to drop it. Falling through would park this on the
        // backoff curve as if the box were merely offline.
        conn->markDisconnected();
        retryAttempts_.remove(conn->id());
        lastFailure_.remove(conn->id());
        emitErrorIfUserInitiated(intent, identityChangedMsg());
        return;
    case reducer::RestVerdict::Ok:
    case reducer::RestVerdict::ShuttingDown:
    case reducer::RestVerdict::Unreachable:
    case reducer::RestVerdict::ServerError:
        // Unreachable, 503 or malformed: park and back off.
        if (intent == ConnectIntent::UserInitiated) {
            conn->markDisconnected();
            emit connectionEvent(makeError(unreachableMsgFor(resp.failure)));
        } else {
            conn->markStale();
        }
        scheduleRetry(server, intent, resp.failure);
        return;
    }
}

void WifiConnectionManager::onSessionVersionRefused(WifiConnection* conn,
                                                    const models::DiscoveredServer& server,
                                                    ConnectIntent intent,
                                                    const models::SessionResponse& resp) {
    const auto negotiated =
        reducer::settleRejected(resp.supportedProtocol, resp.supportedProtocolMin);
    if (negotiated.verdict == reducer::ProtocolVerdict::RetryLower) {
        // The ranges still overlap: re-offer the satellite's ceiling
        // rather than dead-ending the user on "update something".
        // The lowered offer sticks to this connection, so the retry
        // does not repeat the rejected number.
        conn->setOfferedProtocolVersion(negotiated.settledVersion);
        conn->markStale();
        scheduleRetry(server, intent, resp.failure);
        return;
    }
    // The row keeps saying which end must update after the attempt
    // is torn down; an unreadable 409 leaves the chip alone.
    conn->setProtocolCompat(reducer::compatForOutcome(negotiated));
    conn->markDisconnected();
    emitErrorIfUserInitiated(intent, versionMsgFor(negotiated.verdict));
}

void WifiConnectionManager::startSession(WifiConnection* conn,
                                         const models::DiscoveredServer& server,
                                         const std::shared_ptr<SatelliteClient>& client,
                                         const QString& connectionId,
                                         const models::SessionResponse& resp,
                                         const SessionMaterial& material) {
    const QString id = conn->id();
    // The SETTLED version, not the offered one: a pre-versioning
    // satellite echoes 1 whatever we asked for, and the 0x000C frame
    // shape follows the echo.
    const auto negotiated = reducer::settleAccepted(resp.protocolVersion);
    conn->setSettledProtocolVersion(negotiated.settledVersion, negotiated.satelliteBehind);
    conn->setProtocolCompat(reducer::compatForOutcome(negotiated));
    client->setConnectionParams(material.token, material.sessionKey, negotiated.settledVersion);
    store_->remember(server);
    retryAttempts_.remove(id);
    lastFailure_.remove(id);

    conn->markConnected(
        client, connectionId, resp.epoch, resp.mouseControl.granted,
        /*onDead=*/
        [this, id, server] {
            disconnect(id);
            scheduleRetry(server, ConnectIntent::RetryAfterDeath);
        },
        /*onClose=*/
        [this, id, server](std::uint8_t reason) {
            if (auto* c = connections_.value(id, nullptr)) { handleServerClose(c, server, reason); }
        },
        /*onReconcile=*/
        [this, id, server] {
            if (auto* c = connections_.value(id, nullptr)) { reconcile(c, server); }
        },
        /*onRekey=*/
        [this, id, server] {
            if (auto* c = connections_.value(id, nullptr)) { rekey(c, server); }
        });
    conn->applyResults(resp.controllers);
    probeHostAudio(id, server);
}

// The reply applied what was SENT, so a slot the user changed during the round trip is removed or
// re-sent on its own route.
void WifiConnectionManager::convergeLateSlots(WifiConnection* conn,
                                              const std::vector<reducer::DesiredSlot>& sent) {
    const QString id = conn->id();
    const auto converge =
        reducer::lateSlotConverge(sent, descriptorsToDesired(conn->desiredDescriptors()));
    for (std::uint8_t ctrlIdx : converge.removes) { deleteSlot(id, ctrlIdx); }
    for (std::uint8_t ctrlIdx : converge.resyncs) {
        const QString slotId = conn->slotIdForIndex(ctrlIdx);
        if (!slotId.isEmpty()) { syncSlot(id, slotId); }
    }
}

void WifiConnectionManager::reconcile(WifiConnection* conn,
                                      const models::DiscoveredServer& server) {
    const QString id = conn->id();
    if (conn->state() != SessionState::Live) { return; }
    const auto connId = conn->connectionId();
    if (!connId.has_value()) { return; }
    auto client = conn->client();
    if (!client) { return; }
    if (!reducer::reconcileNeeded(client->serverEpoch(), client->serverBitmap(),
                                  conn->lastAppliedEpoch(), conn->registeredBitmap())) {
        return;
    }
    if (reconcileInFlight_.contains(id)) { return; }
    reconcileInFlight_.insert(id);
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) {
        reconcileInFlight_.remove(id);
        return;
    }
    http_->getSession(server.ip, server.httpPort, *connId, deviceId_, creds->proof,
                      [this, id, server](const models::SessionViewDto& view) {
                          reconcileInFlight_.remove(id);
                          auto* c = connections_.value(id, nullptr);
                          if (c == nullptr || c->state() != SessionState::Live) { return; }
                          if (view.unauthorized()) {
                              onTerminalAuthFailure(c, id, ConnectIntent::RetryAfterDeath);
                              return;
                          }
                          if (!view.reachable || view.httpStatus < 200 || view.httpStatus > 299) {
                              return;
                          }
                          if (view.connectionId == c->connectionId() &&
                              c->matchesAppliedView(view)) {
                              // Benign drift, e.g. our own standalone PUT raced an ack.
                              c->adoptEpoch(view.epoch);
                              return;
                          }
                          // Tear the UDP tuple down first, since the converging PUT
                          // rotates the token and key.
                          c->markDisconnected();
                          c->markConnecting();
                          openSession(c, server, ConnectIntent::RetryAfterDeath);
                      });
}

// Same socket, fresh token and key, counters back to 1, so the hot path never blips. connectionId
// is stable across PUTs, so the id and slot state carry over.
void WifiConnectionManager::adoptRekey(WifiConnection* c, const QString& id,
                                       const std::shared_ptr<SatelliteClient>& client,
                                       const models::SessionResponse& resp,
                                       const SessionMaterial& material) {
    // A re-PUT settles again: the satellite could have been upgraded under a live session, and the
    // frame shape must follow the answer it just gave, not the one it gave at connect.
    const auto negotiated = reducer::settleAccepted(resp.protocolVersion);
    c->setSettledProtocolVersion(negotiated.settledVersion, negotiated.satelliteBehind);
    c->setProtocolCompat(reducer::compatForOutcome(negotiated));
    client->setConnectionParams(material.token, material.sessionKey, negotiated.settledVersion);
    // Otherwise the next enriched ack would read as drift.
    c->adoptEpoch(resp.epoch);
    // The satellite could have been upgraded or re-switched under the live session, same reason
    // the protocol version re-settles above.
    probeHostAudio(id, c->server());
}

// Failures stay silent: heartbeat death and terminal-auth already surface them, and a session that
// truly exhausts self-heals via the death retry.
void WifiConnectionManager::onRekeyReply(const QString& id,
                                         const std::shared_ptr<SatelliteClient>& client,
                                         const std::array<std::uint8_t, 32>& pairingKey,
                                         const models::SessionResponse& resp, bool pinMismatch) {
    auto* c = connections_.value(id, nullptr);
    if (c == nullptr) { return; }

    using reducer::RestVerdict;
    const RestVerdict verdict = reducer::classifyRest(restReplyOf(resp, pinMismatch));
    if (verdict == RestVerdict::Unauthorized) {
        onTerminalAuthFailure(c, id, ConnectIntent::RetryAfterDeath);
        return;
    }
    // If a death and reconnect replaced the session mid-flight, applying this material would re-arm
    // the dead client and stamp a stale epoch onto the new session.
    if (c->state() != SessionState::Live || c->client() != client) { return; }
    if (verdict != RestVerdict::Ok) { return; }

    // Missing or malformed material leaves the live session on its current key.
    const auto material = sessionMaterialFrom(resp, pairingKey);
    if (!material.has_value()) { return; }
    adoptRekey(c, id, client, resp, *material);
}

void WifiConnectionManager::rekey(WifiConnection* conn, const models::DiscoveredServer& server) {
    if (conn->state() != SessionState::Live) { return; }
    const auto client = conn->client();
    if (!client) { return; }
    const QString id = conn->id();
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    const auto pairingKey = creds->pairingKey;
    http_->putSession(
        server.ip, server.httpPort, deviceId_, deviceName_, creds->proof,
        conn->desiredDescriptors(), conn->wantsMouseControl(), conn->offeredProtocolVersion(),
        [this, id, client, pairingKey](const models::SessionResponse& resp, bool pinMismatch) {
            onRekeyReply(id, client, pairingKey, resp, pinMismatch);
        });
}

void WifiConnectionManager::probeHostAudio(const QString& id,
                                           const models::DiscoveredServer& server) {
    // Unauthenticated read; a failure keeps the conservative "no audio"
    // default rather than surfacing anything, because the absence of a verdict
    // and a verdict of no are deliberately the same state.
    http_->getCapabilities(
        server.ip, server.httpPort, [this, id](const models::CapabilitiesDto& caps) {
            auto* c = connections_.value(id, nullptr);
            if (c == nullptr || c->state() != SessionState::Live) { return; }
            if (!caps.reachable || caps.httpStatus < 200 || caps.httpStatus > 299) { return; }
            const auto verdict = reducer::resolveHostControllerAudio(caps);
            c->setHostControllerAudio(verdict.mic, verdict.speaker, verdict.hapticAudio);
        });
}

void WifiConnectionManager::syncSlot(const QString& id, const QString& slotId) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr || conn->state() != SessionState::Live) { return; }
    const auto connId = conn->connectionId();
    if (!connId.has_value()) { return; }
    const auto descriptor = conn->descriptorFor(slotId);
    if (!descriptor.has_value()) { return; }
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    const models::DiscoveredServer server = conn->server();
    http_->putController(server.ip, server.httpPort, *connId, deviceId_, creds->proof, *descriptor,
                         [this, id, server](const models::ControllerPutResponse& resp) {
                             auto* c = connections_.value(id, nullptr);
                             if (c == nullptr) { return; }
                             if (resp.unauthorized()) {
                                 onTerminalAuthFailure(c, id, ConnectIntent::RetryAfterDeath);
                                 return;
                             }
                             if (!resp.controller.has_value()) {
                                 // 404: the session died under us, and the alive-poll
                                 // and close-notify paths own recovery.
                                 return;
                             }
                             c->adoptEpoch(resp.epoch);
                             c->applyResults(QList<models::ControllerApplyDto>{*resp.controller});
                             // The grant is only computed at session PUT, so a
                             // mouse-mode toggle needs the whole session converged.
                             if (c->wantsMouseControl() != c->mouseControlGranted()) {
                                 reconcile(c, server);
                             }
                         });
}

void WifiConnectionManager::deleteSlot(const QString& id, int ctrlIdx) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto connId = conn->connectionId();
    if (!connId.has_value()) { return; }
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    const models::DiscoveredServer server = conn->server();
    http_->deleteController(server.ip, server.httpPort, *connId, ctrlIdx, deviceId_, creds->proof,
                            [this, id](const models::ControllerPutResponse& resp) {
                                auto* c = connections_.value(id, nullptr);
                                if (c == nullptr) { return; }
                                if (!resp.error.has_value()) { c->adoptEpoch(resp.epoch); }
                            });
}

void WifiConnectionManager::handleServerClose(WifiConnection* conn,
                                              const models::DiscoveredServer& server,
                                              std::uint8_t reason) {
    const QString id = conn->id();
    switch (reducer::closeActionForReason(reason)) {
    case reducer::CloseAction::DropKeyRePair:
        // unpaired: trust revoked.
        conn->markStale();
        store_->forgetKey(id);
        break;
    case reducer::CloseAction::StayDown:
        // replaced: a newer PUT already owns the session.
        conn->markDisconnected();
        break;
    case reducer::CloseAction::RetryBackoff:
        // shutdown or kicked, both transient.
        conn->markDisconnected();
        scheduleRetry(server, ConnectIntent::RetryAfterDeath);
        break;
    }
}

void WifiConnectionManager::scheduleRetry(const models::DiscoveredServer& server,
                                          ConnectIntent intent, reducer::TransportFailure failure) {
    if (intent == ConnectIntent::UserInitiated) { return; }
    const QString id = server.id();
    const int attempt = retryAttempts_.value(id, 0) + 1;
    retryAttempts_.insert(id, attempt);
    const auto delay = reducer::backoffDelayMs(attempt);
    // A remembered satellite that is simply switched off retries forever, so the
    // cause is logged when it CHANGES and not on every tick. Without the guard a
    // machine left running overnight writes one line a minute for the same fact;
    // with it, an off satellite costs exactly one line, and a satellite that goes
    // from refused to unreachable — the tell for a box leaving the network — still
    // announces itself.
    const auto previous = lastFailure_.value(id, reducer::TransportFailure::None);
    if (failure != previous) {
        lastFailure_.insert(id, failure);
        qCInfo(lcNet, "satellite %s: %s; retrying in %llds", qUtf8Printable(id),
               failureName(failure), static_cast<long long>(delay / 1000));
    } else {
        qCDebug(lcNet, "satellite %s: still %s; retry %d in %llds", qUtf8Printable(id),
                failureName(failure), attempt, static_cast<long long>(delay / 1000));
    }
    const std::uint64_t generation = retryGeneration_.value(id, 0);
    QTimer::singleShot(static_cast<int>(delay), this,
                       [this, id, server, generation] { onRetryDue(id, server, generation); });
}

void WifiConnectionManager::onRetryDue(const QString& id, const models::DiscoveredServer& server,
                                       std::uint64_t generation) {
    // A disconnect since this retry was armed ended what it was retrying.
    const bool disconnectedSince = retryGeneration_.value(id, 0) != generation;
    if (disconnectedSince) { return; }
    auto* c = connections_.value(id, nullptr);
    if (c == nullptr) { return; }
    // Retry only from a settled state: a user-driven reconnect or forget in
    // the interim moved it out, and clobbering that would fight the user.
    if (c->state() != SessionState::Idle && c->state() != SessionState::Stale) { return; }
    // Idempotent, and on completion it persists any new IP and re-runs
    // autoReconnectAll, so a box that moved DHCP leases reconnects on its own
    // without the user opening Manage and pressing Scan.
    startDiscovery();
    // The direct attempt below still runs, so a satellite discovery cannot
    // reach (mDNS and broadcast blocked on the segment) is not left waiting on
    // a scan that may find nothing.
    models::DiscoveredServer target = server;
    for (const auto& r : store_->remembered()) {
        if (r.id == id) {
            target = r.toDiscovered();
            break;
        }
    }
    connectTo(target, ConnectIntent::RetryAfterDeath);
}

void WifiConnectionManager::onTerminalAuthFailure(WifiConnection* conn, const QString& id,
                                                  ConnectIntent intent) {
    // 401 NOT_PAIRED / BAD_PROOF, or no usable key at all. Terminal by contract,
    // so the retry curve stops here rather than hammering a revoked device.
    conn->markStale();
    store_->forgetKey(id);
    retryAttempts_.remove(id);
    lastFailure_.remove(id);
    emitErrorIfUserInitiated(intent, rePairMsg());
}

void WifiConnectionManager::emitErrorIfUserInitiated(ConnectIntent intent, const QString& message) {
    if (intent == ConnectIntent::UserInitiated) { emit connectionEvent(makeError(message)); }
}

void WifiConnectionManager::markStale(const QString& id) {
    if (auto* conn = connections_.value(id, nullptr)) { conn->markStale(); }
}

void WifiConnectionManager::disconnect(const QString& id) {
    // First, and whether or not the connection is still here: every silent retry armed before this
    // point is now out of date. One armed after it, a death's own, still runs.
    const std::uint64_t nextGeneration = retryGeneration_.value(id, 0) + 1;
    retryGeneration_.insert(id, nextGeneration);
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto server = conn->server();
    const auto cid = conn->connectionId();
    conn->markDisconnected();
    if (cid.has_value()) { releaseSession(id, server, *cid); }
}

void WifiConnectionManager::disconnectByUser(const QString& id) {
    heldByUser_.insert(id);
    disconnect(id);
}

void WifiConnectionManager::releaseSession(const QString& id,
                                           const models::DiscoveredServer& server,
                                           const QString& connectionId) {
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    // Best-effort only: the local side already treats the session as gone.
    http_->deleteSession(server.ip, server.httpPort, connectionId, deviceId_, creds->proof,
                         [](int, bool, const QString&) {});
}

void WifiConnectionManager::handBackLateGrant(const QString& id,
                                              const models::DiscoveredServer& server,
                                              const models::SessionResponse& resp) {
    if (resp.connectionId.has_value()) { releaseSession(id, server, *resp.connectionId); }
}

void WifiConnectionManager::forget(const QString& id) {
    // An approval request still aimed at this satellite would re-create it, keyed, when it lands.
    const bool anApprovalRequestAimsAtIt = reverseServer_.id() == id;
    if (anApprovalRequestAimsAtIt) { cancelReversePairing(); }
    auto* conn = connections_.value(id, nullptr);
    // Self-unpair BEFORE dropping the key, since the proof needs it. Otherwise a
    // forgotten dish leaves a paired ghost row on the satellite.
    if (conn != nullptr) {
        const auto server = conn->server();
        const auto creds = credentialsFor(id);
        if (creds.has_value()) {
            http_->unpair(server.ip, server.httpPort, deviceId_, creds->proof,
                          [](int, bool, const QString&) {});
        }
        // The store drops a pin through the remembered row, and a satellite never remembered has
        // none: its pin, left by a handshake that never led to a key, would outlive the Forget.
        store_->facade().pins().forget(server.ip);
    }
    disconnect(id);
    store_->forget(id);
    retryAttempts_.remove(id);
    lastFailure_.remove(id);
    heldByUser_.remove(id);
    reconcileInFlight_.remove(id);
    if (auto* taken = connections_.take(id)) {
        taken->deleteLater();
        emit poolChanged();
    }
}

void WifiConnectionManager::prepareForSleep() {
    // Snapshot the keys: disconnect() fans out through poolChanged into the
    // hub's rebuild, which reshapes connections_ under a live iterator.
    const auto ids = connections_.keys();
    for (const auto& id : ids) { disconnect(id); }
}

void WifiConnectionManager::resumeFromSleep() {
    // A backoff curve armed before the suspend is measuring wall clock the
    // machine spent asleep, so the first attempt after a resume starts over.
    retryAttempts_.clear();
    lastFailure_.clear();
    // Rescan before reconnecting: a laptop that resumes on another network has
    // a stale remembered IP, and only discovery can relearn it.
    startDiscovery();
    autoReconnectAll();
}

void WifiConnectionManager::autoReconnectAll() {
    for (const auto& r : store_->remembered()) {
        auto* existing = connections_.value(r.id, nullptr);
        if (existing == nullptr || existing->state() != SessionState::Live) {
            connectTo(r.toDiscovered(), ConnectIntent::AutoReconnect);
        }
    }
}

} // namespace dish::net
