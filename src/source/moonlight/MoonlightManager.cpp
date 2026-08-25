// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightManager.h"

#include "source/moonlight/MoonlightDiscovery.h"

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
            }
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
        if (const auto* session = sessions_.value(host.uuid, nullptr)) {
            row.link = session->linkState();
        }
        if (const auto it = discovered_.constFind(host.uuid); it != discovered_.constEnd()) {
            row.discovered = true;
        }
        seen.insert(host.uuid);
        out.append(row);
    }
    for (auto it = discovered_.constBegin(); it != discovered_.constEnd(); ++it) {
        if (seen.contains(it.key())) { continue; }
        out.append(it.value());
    }
    std::sort(out.begin(), out.end(),
              [](const MoonlightRow& a, const MoonlightRow& b) { return a.name < b.name; });
    return out;
}

void MoonlightManager::startDiscovery() {
    if (scanning_) { return; }
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
        // Until serverinfo returns the real uuid, key on address; a later
        // pairing rekeys the row to the host uuid.
        const QString key = synthUuidForAddress(found.address);
        MoonlightRow row;
        row.uuid = key;
        row.name = found.name;
        row.address = found.address;
        row.discovered = true;
        discovered_.insert(key, row);
    }
    emit rowsChanged();
}

void MoonlightManager::addManualHost(const QString& address, int httpPort, int httpsPort) {
    const QString key = synthUuidForAddress(address);
    MoonlightRow row;
    row.uuid = key;
    row.name = address;
    row.address = address;
    row.discovered = true;
    discovered_.insert(key, row);
    // Persist the ports on a stub host so a later pair() has them.
    repository::MoonlightHost stub;
    stub.uuid = key;
    stub.name = address;
    stub.address = address;
    stub.httpPort = httpPort;
    stub.httpsPort = httpsPort;
    hostRepo_.upsert(stub);
    emit rowsChanged();
}

void MoonlightManager::pair(const QString& uuid) {
    ensureIdentityLoaded();
    if (!identityReady_) {
        emit pairingFinished(uuid, false, QStringLiteral("crypto"));
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
    if (address.isEmpty()) {
        emit pairingFinished(uuid, false, QStringLiteral("unreachable"));
        return;
    }
    const auto identity = identityRepo_.identity();
    if (!identity) {
        emit pairingFinished(uuid, false, QStringLiteral("crypto"));
        return;
    }
    pairingFlow_->start(uuid, address, httpPort, httpsPort, identity->certPem,
                        identity->privateKeyPem, deviceName_);
    emit pairingChanged();
}

void MoonlightManager::cancelPairing() {
    pairingFlow_->cancel();
    emit pairingChanged();
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
                     [this, uuid](const QString& reasonToken) {
                         emit sessionFailed(uuid, reasonToken);
                         emit rowsChanged();
                     });
    session->setRumbleHandler([this, uuid](std::uint16_t low, std::uint16_t high) {
        if (rumbleSink_) { rumbleSink_(uuid, low, high); }
    });
    session->setLedHandler([this, uuid](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (ledSink_) { ledSink_(uuid, r, g, b); }
    });
}

void MoonlightManager::connectHost(const QString& uuid, const QString& appId,
                                   std::uint8_t emulatedType, std::uint8_t capabilities) {
    ensureIdentityLoaded();
    const auto host = hostRepo_.get(uuid);
    if (!host || !host->paired()) { return; }
    auto* session = ensureSession(*host);
    session->start(appId, emulatedType, capabilities);
    emit rowsChanged();
}

void MoonlightManager::disconnect(const QString& uuid) {
    if (auto* session = sessions_.value(uuid, nullptr)) {
        session->stop();
        emit rowsChanged();
    }
}

void MoonlightManager::forget(const QString& uuid) {
    if (auto* session = sessions_.take(uuid)) {
        session->stop();
        session->deleteLater();
    }
    hostRepo_.remove(uuid);
    discovered_.remove(uuid);
    emit rowsChanged();
}

void MoonlightManager::setLastApp(const QString& uuid, const QString& appId,
                                  const QString& appName) {
    if (auto host = hostRepo_.get(uuid)) {
        host->lastAppId = appId;
        host->lastAppName = appName;
        hostRepo_.upsert(*host);
        emit rowsChanged();
    }
}

void MoonlightManager::setControllerType(const QString& uuid, int type) {
    if (auto host = hostRepo_.get(uuid)) {
        host->controllerType = type;
        hostRepo_.upsert(*host);
        emit rowsChanged();
    }
}

MoonlightSession* MoonlightManager::session(const QString& uuid) const {
    return sessions_.value(uuid, nullptr);
}

} // namespace dish::source::moon
