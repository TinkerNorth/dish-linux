// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightManager.h"

#include "core/moonlight/MoonlightXml.h"
#include "source/moonlight/MoonlightDiscovery.h"
#include "source/moonlight/MoonlightLog.h"

#include <QFutureWatcher>
#include <QHostInfo>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>

namespace dish::source::moon {
namespace {

QString synthUuidForAddress(const QString& address) {
    return QStringLiteral("addr:%1").arg(address);
}

// Sunshine's own "Desktop" app id, and the fallback when the host offered no
// list we could read. The host still picks its default if it disagrees.
constexpr const char* kDefaultAppId = "1";

} // namespace

MoonlightManager::MoonlightManager(const std::shared_ptr<QSettings>& settings, QObject* parent)
    : QObject(parent), settings_(settings), identityRepo_(settings), hostRepo_(settings),
      http_(new MoonlightHttp(this)), pairingFlow_(std::make_unique<MoonlightPairingFlow>(http_)) {
    deviceName_ = QStringLiteral("Dish (%1)").arg(QHostInfo::localHostName());

    QObject::connect(pairingFlow_.get(), &MoonlightPairingFlow::pinReady, this,
                     [this](const QString&) { emit pairingChanged(); });
    QObject::connect(
        pairingFlow_.get(), &MoonlightPairingFlow::finished, this,
        [this](bool ok, const QString& reasonToken, const QString& serverCertPem) {
            const QString uuid = pairingFlow_->hostUuid();
            if (ok) {
                // Promote the discovered row to a remembered, paired host.
                auto stored = hostRepo_.get(uuid);
                repository::MoonlightHost host = stored.value_or(repository::MoonlightHost{});
                host.uuid = uuid;
                if (const auto it = discovered_.constFind(uuid); it != discovered_.constEnd()) {
                    if (host.name.isEmpty()) { host.name = it->name; }
                    host.address = it->address;
                }
                host.serverCertPem = serverCertPem;
                hostRepo_.upsert(host);
                pairingRefusedUuid_.clear();
                // A successful pairing IS a verification: the handshake proved
                // the trust the probe would have asked about.
                HostProbe& probe = probes_[uuid];
                probe.inFlight = false;
                probe.answered = true;
                probe.paired = true;
                probe.identityChanged = false;
                probe.trustRejected = false;
            } else {
                pairingRefusedUuid_ = uuid;
            }
            qCInfo(lcMoon) << "pairing with" << uuid << (ok ? "succeeded" : "failed")
                           << reasonToken;
            emit pairingChanged();
            emit pairingFinished(uuid, ok, reasonToken);
            emit rowsChanged();
        });
}

MoonlightManager::~MoonlightManager() {
    for (auto* session : sessions_) { session->stop(); }
}

void MoonlightManager::ensureIdentityLoaded() {
    if (identityReady_) { return; }
    const auto identity = identityRepo_.ensureIdentity();
    if (!identity) { return; }
    http_->setIdentity(identity->certPem, identity->privateKeyPem, identity->uniqueId);
    identityReady_ = true;
}

QList<MoonlightRow> MoonlightManager::rows() const {
    QList<MoonlightRow> out;
    QSet<QString> seen;
    for (const auto& host : hostRepo_.all()) {
        MoonlightRow row;
        row.uuid = host.uuid;
        row.name = host.name.isEmpty() ? host.address : host.name;
        row.address = host.address;
        row.paired = host.paired();
        row.lastAppId = host.lastAppId;
        row.lastAppName = host.lastAppName;
        row.controllerType = host.controllerType;
        const auto* session = sessions_.value(host.uuid, nullptr);
        if (session != nullptr) {
            row.link = session->linkState();
            row.controllers = static_cast<int>(session->controllerCount());
        }
        if (const auto it = discovered_.constFind(host.uuid); it != discovered_.constEnd()) {
            row.discovered = true;
        }
        const auto inputs = uiInputs(host.uuid, QString());
        row.trust = moonlight::hostTrust(inputs);
        row.phase = pairingFlow_->active() && pairingFlow_->hostUuid() == host.uuid
                        ? moonlight::HostPhase::Pairing
                        : moonlight::hostPhaseFor(session != nullptr ? session->machineState()
                                                                     : moonlight::SessionState{},
                                                  row.trust != moonlight::HostTrust::NotPaired,
                                                  session != nullptr && session->everStarted());
        seen.insert(host.uuid);
        out.append(row);
    }
    for (auto it = discovered_.constBegin(); it != discovered_.constEnd(); ++it) {
        if (seen.contains(it.key())) { continue; }
        MoonlightRow row = it.value();
        const auto inputs = uiInputs(row.uuid, QString());
        row.trust = moonlight::hostTrust(inputs);
        row.phase = pairingFlow_->active() && pairingFlow_->hostUuid() == row.uuid
                        ? moonlight::HostPhase::Pairing
                        : moonlight::HostPhase::Idle;
        out.append(row);
    }
    std::sort(out.begin(), out.end(),
              [](const MoonlightRow& a, const MoonlightRow& b) { return a.name < b.name; });
    return out;
}

bool MoonlightManager::knows(const QString& uuid) const {
    if (uuid.isEmpty()) { return false; }
    return hostRepo_.get(uuid).has_value() || discovered_.contains(uuid);
}

std::optional<MoonlightRow> MoonlightManager::row(const QString& uuid) const {
    for (const auto& row : rows()) {
        if (row.uuid == uuid) { return row; }
    }
    return std::nullopt;
}

void MoonlightManager::startDiscovery() {
    if (scanning_) {
        qCDebug(lcMoon) << "discovery already running; coalesced";
        return;
    }
    scanning_ = true;
    emit scanningChanged();
    auto* watcher = new QFutureWatcher<QList<DiscoveredMoonlightHost>>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        onDiscovered(watcher->result());
        watcher->deleteLater();
        scanning_ = false;
        emit scanningChanged();
    });
    watcher->setFuture(QtConcurrent::run([] { return MoonlightDiscovery::discover(); }));
}

void MoonlightManager::onDiscovered(const QList<DiscoveredMoonlightHost>& hosts) {
    for (const auto& found : hosts) {
        // ONE HOST, ONE ID, and the address is it. mDNS here advertises no
        // uniqueid, so the address is the only identifier both the found and
        // the typed-in routes have on first contact; keying on it everywhere
        // means a record, a pin and a binding never disagree about which host
        // they mean. serverinfo's uuid is read for the REPLACED check and
        // never promoted to a key, because a rekey would have to migrate all
        // three at once. The cost is that a host which moves address arrives
        // as a new row, which is the same trade the satellite pool makes.
        //
        // A sweep MERGES. Nothing here removes an entry a previous sweep found,
        // so a pass that answers with less than the last one cannot delete a
        // host the user is in the middle of using.
        const QString key = synthUuidForAddress(found.address);
        MoonlightRow row;
        row.uuid = key;
        row.name = found.name;
        row.address = found.address;
        row.discovered = true;
        discovered_.insert(key, row);
    }
    qCInfo(lcMoon) << "discovery found" << hosts.size() << "host(s)";
    emit rowsChanged();
}

void MoonlightManager::addManualHost(const QString& address, const QString& name, int httpPort,
                                     int httpsPort) {
    const QString key = synthUuidForAddress(address);
    MoonlightRow row;
    row.uuid = key;
    row.name = name.isEmpty() ? address : name;
    row.address = address;
    row.discovered = true;
    discovered_.insert(key, row);
    // Persist the ports on a stub host so a later pair() has them.
    repository::MoonlightHost stub;
    stub.uuid = key;
    stub.name = row.name;
    stub.address = address;
    stub.httpPort = httpPort;
    stub.httpsPort = httpsPort;
    hostRepo_.upsert(stub);
    qCInfo(lcMoon) << "added host by address" << address << httpPort << httpsPort;
    emit rowsChanged();
}

// A Pair that ends before the wire is still an ANSWER. It records the refusal
// so hostTrust and sessionUiState can render PairingRefused, says why in the
// log, and re-emits the row set; without all three the PIN sheet sits on an
// indeterminate spinner and four empty digit cells forever, which is exactly
// what "I pressed Pair and nothing happened" looks like from the outside.
void MoonlightManager::refusePairing(const QString& uuid, const QString& reasonToken) {
    pairingRefusedUuid_ = uuid;
    pairingRefusedReason_ = reasonToken;
    qCWarning(lcMoon) << "pairing with" << uuid << "refused before the wire:" << reasonToken;
    emit pairingChanged();
    emit pairingFinished(uuid, false, reasonToken);
    emit rowsChanged();
}

void MoonlightManager::rememberDestination(const QString& uuid) {
    if (uuid.isEmpty() || hostRepo_.get(uuid)) { return; }
    const auto it = discovered_.constFind(uuid);
    if (it == discovered_.constEnd()) {
        qCWarning(lcMoon) << "cannot remember" << uuid << ": no address on file for it";
        return;
    }
    // Unpaired on purpose: this records INTEREST, not trust. The anchor is
    // still only ever written by a pairing handshake that verified it.
    repository::MoonlightHost host;
    host.uuid = uuid;
    host.name = it->name;
    host.address = it->address;
    hostRepo_.upsert(host);
    qCInfo(lcMoon) << "remembered" << uuid << "at" << it->address << "as a binding destination";
}

void MoonlightManager::pair(const QString& uuid) {
    ensureIdentityLoaded();
    pairingRefusedUuid_.clear();
    pairingRefusedReason_.clear();
    if (uuid.isEmpty()) {
        qCWarning(lcMoon) << "pair called with no host";
        return;
    }
    if (!identityReady_) {
        refusePairing(uuid, QStringLiteral("crypto"));
        return;
    }
    QString address;
    int httpPort = 47989;
    int httpsPort = 47984;
    if (const auto host = hostRepo_.get(uuid)) {
        address = host->address;
        httpPort = host->httpPort;
        httpsPort = host->httpsPort;
    } else if (const auto it = discovered_.constFind(uuid); it != discovered_.constEnd()) {
        address = it->address;
    }
    // The forgotten-then-paired case: a Forget drops both the remembered row
    // and the discovered one, so a sheet still holding the old id has nowhere
    // to dial. It has to SAY so rather than open on a PIN that never arrives.
    if (address.isEmpty()) {
        refusePairing(uuid, QStringLiteral("unreachable"));
        return;
    }
    const auto identity = identityRepo_.identity();
    if (!identity) {
        refusePairing(uuid, QStringLiteral("crypto"));
        return;
    }
    qCInfo(lcMoon) << "pairing with" << uuid << "at" << address << httpPort << httpsPort;
    pairingFlow_->start(uuid, address, httpPort, httpsPort, identity->certPem,
                        identity->privateKeyPem, deviceName_);
    emit pairingChanged();
}

void MoonlightManager::cancelPairing() {
    qCInfo(lcMoon) << "pairing with" << pairingFlow_->hostUuid() << "cancelled by the user";
    pairingFlow_->cancel();
    pairingRefusedUuid_.clear();
    pairingRefusedReason_.clear();
    emit pairingChanged();
}

bool MoonlightManager::pairingRefused(const QString& uuid) const {
    return !uuid.isEmpty() && pairingRefusedUuid_ == uuid;
}

QString MoonlightManager::pairingRefusedReason(const QString& uuid) const {
    return pairingRefused(uuid) ? pairingRefusedReason_ : QString();
}

void MoonlightManager::probe(const QString& uuid) {
    QString address;
    int httpPort = 47989;
    const auto stored = hostRepo_.get(uuid);
    if (stored) {
        address = stored->address;
        httpPort = stored->httpPort;
    } else if (const auto it = discovered_.constFind(uuid); it != discovered_.constEnd()) {
        address = it->address;
    }
    if (address.isEmpty()) {
        // Nothing to ask, so probeFinished has to fire anyway: a caller that
        // waits for it (the host screen re-probes every row on open) would
        // otherwise sit on Checking for a host that no longer exists.
        qCWarning(lcMoon) << "probe of" << uuid << "skipped: no address on file";
        emit probeFinished(uuid);
        return;
    }

    HostProbe& probe = probes_[uuid];
    if (probe.inFlight) {
        qCDebug(lcMoon) << "probe of" << address << "already in flight; coalesced";
        return;
    }
    probe.inFlight = true;
    emit rowsChanged();

    // PLAINTEXT, deliberately: PairStatus is the one thing an unpaired client
    // can read, and `currentgame` / `state` from this port describe nobody, so
    // they are not read here at all.
    ensureIdentityLoaded();
    const QString rememberedUuid = stored ? stored->uuid : QString();
    const quint64 epoch = epochOf(uuid);
    http_->getPlain(
        address, httpPort, QStringLiteral("/serverinfo"), QUrlQuery(),
        [this, uuid, rememberedUuid, address, epoch](int status, const QByteArray& body) {
            if (epochOf(uuid) != epoch) {
                // Forgotten while this was in flight. probes_[uuid] would
                // INSERT, handing a stranger the verdict of the host it used
                // to be, so the answer is dropped instead.
                qCInfo(lcMoon) << "probe reply for" << address << "arrived after a forget";
                return;
            }
            HostProbe& result = probes_[uuid];
            result.inFlight = false;
            const std::optional<moonxml::ServerInfo> info =
                status == 200 ? moonxml::parseServerInfo(body.toStdString())
                              : std::optional<moonxml::ServerInfo>{};
            if (!info) {
                result.answered = false;
                qCInfo(lcMoon) << "probe of" << address << "did not answer; HTTP" << status;
                emit probeFinished(uuid);
                emit rowsChanged();
                return;
            }
            result.answered = true;
            result.paired = info->pairStatus == 1;
            // A uuid we do not recognise means the machine behind the address
            // was reset or replaced, so the stored certificate anchors nothing.
            const QString reported = QString::fromStdString(info->uuid);
            result.identityChanged = !rememberedUuid.isEmpty() && !reported.isEmpty() &&
                                     !rememberedUuid.startsWith(QLatin1String("addr:")) &&
                                     reported != rememberedUuid;
            if (result.paired) { result.trustRejected = false; }
            qCInfo(lcMoon) << "probe of" << address << "paired" << result.paired << "identity"
                           << (result.identityChanged ? "changed" : "same");
            emit probeFinished(uuid);
            emit rowsChanged();
        });
}

void MoonlightManager::refreshApps(const QString& uuid) {
    ensureIdentityLoaded();
    const auto host = hostRepo_.get(uuid);
    AppCache& cache = appCache_[uuid];
    if (!host || !host->paired()) {
        // The app list is HTTPS and paired-only; saying "no apps" here would
        // present a 404 as a fact about the host.
        cache.inFlight = false;
        cache.read = false;
        cache.failed = true;
        qCInfo(lcMoon) << "applist on" << uuid << "not attempted: host is not paired";
        emit appsChanged(uuid);
        return;
    }
    if (cache.inFlight) {
        qCDebug(lcMoon) << "applist on" << host->address << "already in flight; coalesced";
        return;
    }
    cache.inFlight = true;
    cache.failed = false;
    emit appsChanged(uuid);

    const quint64 epoch = epochOf(uuid);
    http_->getTls(host->address, host->httpsPort, QStringLiteral("/applist"), QUrlQuery(),
                  host->serverCertPem,
                  [this, uuid, epoch, address = host->address](int status, const QByteArray& body) {
                      if (epochOf(uuid) != epoch) {
                          // As in probe(): both appCache_ and probes_ below are
                          // written through operator[], so a reply that outlived
                          // a Forget would re-create what the Forget dropped.
                          qCInfo(lcMoon) << "applist reply for" << address << "outlived a forget";
                          return;
                      }
                      AppCache& result = appCache_[uuid];
                      result.inFlight = false;
                      const std::string xml = body.toStdString();
                      const auto refusal = moonxml::parseStatus(xml);
                      if (status != 200 || (refusal && !refusal->ok())) {
                          result.failed = true;
                          // A 401 is the host saying it does not know this
                          // client any more, which is trust lost and not a
                          // transport fault.
                          if (status == 401 || (refusal && refusal->code == 401)) {
                              probes_[uuid].trustRejected = true;
                          }
                          qCWarning(lcMoon) << "applist on" << address << "HTTP" << status;
                          emit appsChanged(uuid);
                          emit rowsChanged();
                          return;
                      }
                      // A reply we could read is proof of trust: the mutual-TLS
                      // handshake behind it is exactly what pairing establishes.
                      HostProbe& probe = probes_[uuid];
                      probe.answered = true;
                      probe.paired = true;
                      probe.trustRejected = false;

                      result.apps.clear();
                      for (const auto& entry : moonxml::parseAppList(xml)) {
                          MoonlightApp app;
                          app.id = QString::fromStdString(entry.id);
                          app.title = QString::fromStdString(entry.title);
                          result.apps.append(app);
                      }
                      result.read = true;
                      result.failed = false;
                      qCInfo(lcMoon)
                          << "applist on" << address << "returned" << result.apps.size() << "apps";
                      emit appsChanged(uuid);
                      emit rowsChanged();
                  });
}

QList<MoonlightApp> MoonlightManager::apps(const QString& uuid) const {
    const auto it = appCache_.constFind(uuid);
    if (it == appCache_.constEnd()) { return {}; }
    return it->apps;
}

MoonlightSession* MoonlightManager::ensureSession(const repository::MoonlightHost& host) {
    if (auto* existing = sessions_.value(host.uuid, nullptr)) { return existing; }
    auto* session = new MoonlightSession(http_, host, this);
    wireSession(session, host.uuid);
    sessions_.insert(host.uuid, session);
    return session;
}

void MoonlightManager::wireSession(MoonlightSession* session, const QString& uuid) {
    QObject::connect(session, &MoonlightSession::linkStateChanged, this,
                     &MoonlightManager::rowsChanged);
    QObject::connect(session, &MoonlightSession::failed, this,
                     [this, session, uuid](const QString& reasonToken) {
                         // A session already out of the table is one forget()
                         // is tearing down. Its verdict is about a host that no
                         // longer exists, and probes_[uuid] would insert it.
                         if (sessions_.value(uuid, nullptr) != session) {
                             qCInfo(lcMoon)
                                 << "session on" << uuid << "failed after a forget:" << reasonToken;
                             return;
                         }
                         qCWarning(lcMoon) << "session on" << uuid << "failed:" << reasonToken;
                         if (reasonToken == QLatin1String("trustLost") ||
                             reasonToken == QLatin1String("notPaired")) {
                             probes_[uuid].trustRejected = true;
                         }
                         emit sessionFailed(uuid, reasonToken);
                         emit rowsChanged();
                     });
    session->setRumbleHandler(
        [this, uuid](std::uint8_t number, std::uint16_t low, std::uint16_t high) {
            auto* live = sessions_.value(uuid, nullptr);
            if (live == nullptr || !rumbleSink_) { return; }
            const QString slotId = live->slotForController(number);
            if (!slotId.isEmpty()) { rumbleSink_(slotId, low, high); }
        });
    session->setLedHandler(
        [this, uuid](std::uint8_t number, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            auto* live = sessions_.value(uuid, nullptr);
            if (live == nullptr || !ledSink_) { return; }
            const QString slotId = live->slotForController(number);
            if (!slotId.isEmpty()) { ledSink_(slotId, r, g, b); }
        });
}

void MoonlightManager::ensureSessionRunning(MoonlightSession* session,
                                            const repository::MoonlightHost& host) {
    if (!moonlight::sessionNeedsStart(session->machineState().phase)) { return; }
    QString appId = host.lastAppId;
    QString appName = host.lastAppName;
    if (appId.isEmpty()) {
        // Whatever the host lists first, which is what the copy promises when
        // the user made no pick; the bare default id only when we read no list.
        const auto listed = apps(host.uuid);
        if (!listed.isEmpty()) {
            appId = listed.front().id;
            appName = listed.front().title;
        } else {
            appId = QString::fromLatin1(kDefaultAppId);
        }
    }
    session->start(appId, appName);
}

std::optional<std::uint8_t>
MoonlightManager::bindController(const QString& slotId, const QString& uuid, int storedType,
                                 const moonlight::SourceCapabilities& source) {
    if (slotId.isEmpty() || uuid.isEmpty()) {
        qCWarning(lcMoon) << "bind refused: slot" << slotId << "host" << uuid;
        return std::nullopt;
    }
    // A slot drives exactly one destination.
    if (const QString prior = bindings_.value(slotId); !prior.isEmpty() && prior != uuid) {
        unbindController(slotId);
    }
    // The four-pad ceiling is a property of the HOST, not of the session, so it
    // is enforced before a session exists too: an unpaired host that already
    // carries four bindings has no room for a fifth either.
    if (bindings_.value(slotId) != uuid) {
        int others = 0;
        for (auto it = bindings_.constBegin(); it != bindings_.constEnd(); ++it) {
            if (it.value() == uuid) { ++others; }
        }
        if (others >= moonlight::kMaxPads) {
            qCWarning(lcMoon) << "host" << uuid << "already carries" << others
                              << "bindings; refusing" << slotId;
            return std::nullopt;
        }
    }
    bindings_.insert(slotId, uuid);

    ensureIdentityLoaded();
    // A destination the user picked stops being a scan result and becomes a
    // record. A binding on a host that lives only in the discovered set would
    // name nothing the moment the sweep that found it is replaced.
    rememberDestination(uuid);
    const auto host = hostRepo_.get(uuid);
    if (!host || !host->paired()) {
        // The binding stands; the session waits for trust. Nothing about the
        // host's state may refuse to record what the user asked for.
        qCInfo(lcMoon) << "binding" << slotId << "to unpaired host" << uuid
                       << "; the session waits for pairing";
        emit rowsChanged();
        return std::nullopt;
    }
    auto* session = ensureSession(*host);
    // Re-binding a slot that already holds a number is a RESTART, not a second
    // pad: the number stands and the session is asked to run again, which is
    // what Reconnect after a drop means.
    auto number = session->controllerNumber(slotId);
    if (!number) { number = session->attachController(slotId, storedType, source); }
    if (!number) {
        // Four pads already ride this host; the binding is not recorded,
        // because there is no controller number for it to use.
        bindings_.remove(slotId);
        qCWarning(lcMoon) << "host" << uuid << "already carries" << session->controllerCount()
                          << "controllers; refusing" << slotId;
        emit rowsChanged();
        return std::nullopt;
    }
    ensureSessionRunning(session, *host);
    qCInfo(lcMoon) << "bound" << slotId << "to" << uuid << "as controller" << *number;
    emit rowsChanged();
    return number;
}

void MoonlightManager::unbindController(const QString& slotId) {
    const QString uuid = bindings_.take(slotId);
    if (uuid.isEmpty()) {
        qCDebug(lcMoon) << "unbind of" << slotId << "is a no-op: no Moonlight binding on it";
        return;
    }
    qCInfo(lcMoon) << "unbinding" << slotId << "from" << uuid;
    auto* session = sessions_.value(uuid, nullptr);
    if (session == nullptr) {
        // A binding that never got a session, which is every binding made
        // before its host was paired. The intent is retired and that is all.
        emit rowsChanged();
        return;
    }
    if (session->detachController(slotId) == 0) {
        // The last controller has left, so nothing is riding the app any more:
        // hand it back rather than strand it on the host.
        session->stop(/*handBackApp=*/true);
    }
    emit rowsChanged();
}

QString MoonlightManager::boundHostFor(const QString& slotId) const {
    return bindings_.value(slotId);
}

QStringList MoonlightManager::boundSlots(const QString& uuid) const {
    QStringList out;
    for (auto it = bindings_.constBegin(); it != bindings_.constEnd(); ++it) {
        if (it.value() == uuid) { out.append(it.key()); }
    }
    out.sort();
    return out;
}

int MoonlightManager::controllerCount(const QString& uuid) const {
    if (const auto* session = sessions_.value(uuid, nullptr)) {
        return static_cast<int>(session->controllerCount());
    }
    return static_cast<int>(boundSlots(uuid).size());
}

std::optional<std::uint8_t> MoonlightManager::controllerNumber(const QString& slotId) const {
    const QString uuid = bindings_.value(slotId);
    if (uuid.isEmpty()) { return std::nullopt; }
    if (const auto* session = sessions_.value(uuid, nullptr)) {
        return session->controllerNumber(slotId);
    }
    return std::nullopt;
}

moonlight::SessionUiInputs MoonlightManager::uiInputs(const QString& uuid,
                                                      const QString& slotId) const {
    moonlight::SessionUiInputs in;
    const auto host = hostRepo_.get(uuid);
    in.remembered = host && host->paired();

    if (const auto it = probes_.constFind(uuid); it != probes_.constEnd()) {
        in.probeAttempted = true;
        in.probeInFlight = it->inFlight;
        in.probeAnswered = it->answered;
        in.paired = it->paired;
        in.identityChanged = it->identityChanged;
        in.trustRejected = it->trustRejected;
    }
    in.pairingActive = pairingFlow_->active() && pairingFlow_->hostUuid() == uuid;
    in.pairingRefused = pairingRefusedUuid_ == uuid && !uuid.isEmpty();

    if (const auto it = appCache_.constFind(uuid); it != appCache_.constEnd()) {
        in.appsInFlight = it->inFlight;
        in.appsRead = it->read;
        in.appsFailed = it->failed;
        in.appCount = static_cast<int>(it->apps.size());
    }

    if (const auto* session = sessions_.value(uuid, nullptr)) {
        const auto& machine = session->machineState();
        in.sessionLive = machine.phase == moonlight::SessionPhase::Streaming;
        in.bindingLive =
            in.sessionLive && !slotId.isEmpty() && session->controllerNumber(slotId).has_value();
        if (machine.phase == moonlight::SessionPhase::Failed) { in.failure = machine.failure; }
    }
    // Every controller on this host except the one being edited, so a binding
    // that already holds a number is never told the host is full.
    int others = 0;
    for (auto it = bindings_.constBegin(); it != bindings_.constEnd(); ++it) {
        if (it.value() == uuid && it.key() != slotId) { ++others; }
    }
    in.otherControllers = others;
    return in;
}

void MoonlightManager::quitHostApp(const QString& uuid) {
    ensureIdentityLoaded();
    const auto host = hostRepo_.get(uuid);
    if (!host || !host->paired()) {
        // /cancel is HTTPS and paired-only, so there is nothing to send.
        qCWarning(lcMoon) << "cancel on" << uuid << "not attempted: host is not paired";
        emit hostAppCancelled(uuid, false);
        return;
    }
    // Our own session first: tearing it down hands the app back through the
    // same /cancel, and leaving it live would race the request.
    if (auto* session = sessions_.value(uuid, nullptr)) {
        if (session->machineState().phase != moonlight::SessionPhase::Idle) {
            session->stop(/*handBackApp=*/true);
            emit hostAppCancelled(uuid, true);
            probe(uuid);
            return;
        }
    }
    http_->getTls(host->address, host->httpsPort, QStringLiteral("/cancel"), QUrlQuery(),
                  host->serverCertPem,
                  [this, uuid, address = host->address](int status, const QByteArray& body) {
                      const auto refusal = moonxml::parseStatus(body.toStdString());
                      const bool ok = status == 200 && (!refusal || refusal->ok());
                      qCInfo(lcMoon) << "cancel on" << address << "HTTP" << status << "host"
                                     << (refusal ? refusal->code : 0) << "->" << ok;
                      emit hostAppCancelled(uuid, ok);
                      emit rowsChanged();
                      // /cancel answers 200 whether or not anything was
                      // running, so success here proves nothing: ask again.
                      probe(uuid);
                  });
}

void MoonlightManager::forget(const QString& uuid) {
    if (uuid.isEmpty()) {
        qCWarning(lcMoon) << "forget called with no host";
        return;
    }
    // THE EPOCH FIRST. Every request already on the wire for this host captured
    // the old one and will now drop its own reply, which is what stops a probe
    // or an applist landing a moment later from re-creating the records the
    // rest of this function removes.
    ++epochs_[uuid];
    // A pairing still walking its phases would finish by upserting the row
    // again, certificate and all: the host list would read empty while the
    // pairing anchor stayed on file, and the next pair would meet a pin the
    // user believes they deleted. cancel() does not emit finished().
    if (pairingFlow_->active() && pairingFlow_->hostUuid() == uuid) {
        qCInfo(lcMoon) << "forget cancels the pairing in flight with" << uuid;
        pairingFlow_->cancel();
    }
    const QStringList dropped = boundSlots(uuid);
    for (const auto& slotId : dropped) { bindings_.remove(slotId); }
    auto* session = sessions_.take(uuid);

    // EVERY RECORD GOES BEFORE THE SESSION IS MADE TO SPEAK. stop() dispatches
    // through the session machine and raises linkStateChanged, which reaches
    // rowsChanged and every surface bound to it while this function would
    // otherwise still be half done: a handler on the far side of that emit
    // would resolve a host that is on its way out, and a probe asked for there
    // would re-insert probes_[uuid] under the epoch this call already bumped,
    // so its own reply would match and write the record back.
    //
    // The pairing anchor lives IN the row, so removing the row removes the pin.
    // The session can be torn down after, because it carries its own COPY of
    // the host record: the /cancel its teardown sends does not read the store.
    hostRepo_.remove(uuid);
    discovered_.remove(uuid);
    probes_.remove(uuid);
    appCache_.remove(uuid);
    if (pairingRefusedUuid_ == uuid) {
        pairingRefusedUuid_.clear();
        pairingRefusedReason_.clear();
    }

    if (session != nullptr) {
        session->stop(/*handBackApp=*/true);
        session->deleteLater();
    }
    qCInfo(lcMoon) << "forgot" << uuid << "and the" << dropped.size() << "bindings it carried";
    emit rowsChanged();
}

void MoonlightManager::setLastApp(const QString& uuid, const QString& appId,
                                  const QString& appName) {
    auto host = hostRepo_.get(uuid);
    if (!host) {
        // Only a REMEMBERED host has somewhere to keep a pick. Dropping it
        // quietly is how an app choice silently fails to stick, so it is said
        // out loud instead.
        qCWarning(lcMoon) << "app pick" << appId << "not stored: no remembered host" << uuid;
        return;
    }
    host->lastAppId = appId;
    host->lastAppName = appName;
    hostRepo_.upsert(*host);
    qCInfo(lcMoon) << "host" << uuid << "will next run" << appId;
    emit rowsChanged();
}

void MoonlightManager::setControllerType(const QString& uuid, int type) {
    auto host = hostRepo_.get(uuid);
    if (!host) {
        qCWarning(lcMoon) << "controller type" << type << "not stored: no remembered host" << uuid;
        return;
    }
    host->controllerType = moonlight::migrateControllerType(type);
    hostRepo_.upsert(*host);
    emit rowsChanged();
}

MoonlightSession* MoonlightManager::session(const QString& uuid) const {
    return sessions_.value(uuid, nullptr);
}

} // namespace dish::source::moon
