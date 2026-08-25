// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MoonlightHostRepository — the remembered Moonlight hosts, one JSON array in
// the shared connection-store QSettings (mirroring RememberedSatelliteRepository
// for the satellite family). A host row persists its pairing anchor — the
// server certificate PEM the pairing handshake verified — so later TLS
// connects pin against it, plus the user's per-host picks (app, emulated
// controller type).

#pragma once

#include "architecture/Repository.h"
#include "core/moonlight/MoonlightProtocol.h"

#include <QHash>
#include <QJsonObject>
#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dish::repository {

struct MoonlightHost {
    // The host's serverinfo uuid — the stable identity a DHCP move keeps.
    QString uuid;
    QString name;
    QString address; // IP or hostname, as discovered or typed
    int httpPort = 47989;
    int httpsPort = 47984;
    // The pairing anchor. Empty means discovered-but-never-paired.
    QString serverCertPem;
    // The user's last app pick; empty until the first launch.
    QString lastAppId;
    QString lastAppName;
    // moonproto::kControllerType*, or kControllerTypeAuto for "match the pad".
    int controllerType = 0xFF;

    bool paired() const { return !serverCertPem.isEmpty(); }

    QJsonObject toJson() const;
    static std::optional<MoonlightHost> fromJson(const QJsonObject& obj);

    bool operator==(const MoonlightHost& o) const {
        return uuid == o.uuid && name == o.name && address == o.address && httpPort == o.httpPort &&
               httpsPort == o.httpsPort && serverCertPem == o.serverCertPem &&
               lastAppId == o.lastAppId && lastAppName == o.lastAppName &&
               controllerType == o.controllerType;
    }
    bool operator!=(const MoonlightHost& o) const { return !(*this == o); }
};

// "Match the pad" sentinel for MoonlightHost::controllerType. Not a wire value:
// the session resolves it against the bound pad before CONTROLLER_ARRIVAL.
inline constexpr int kMoonlightControllerTypeAuto = 0xFF;

class MoonlightHostRepository : public arch::Repository<QString, MoonlightHost> {
  public:
    explicit MoonlightHostRepository(std::shared_ptr<QSettings> settings = nullptr);

    // Insert-or-update keyed on uuid, preserving pairing/pick fields the caller
    // left empty (a re-discovery must not wipe the cert or the app choice).
    void upsert(const MoonlightHost& host);

    std::optional<MoonlightHost> get(const QString& uuid) const override;
    std::vector<MoonlightHost> all() const override;
    void put(const QString& uuid, const MoonlightHost& host) override;
    void remove(const QString& uuid) override;
    void clear() override;

  private:
    // Keyed by the storage key (the uuid), value stored verbatim — the
    // storage key is authoritative, mirroring RememberedSatelliteRepository.
    // Callers hold mutex_.
    QHash<QString, MoonlightHost> load() const;
    void store(const QHash<QString, MoonlightHost>& hosts);

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
