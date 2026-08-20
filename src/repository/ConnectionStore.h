// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// ConnectionStore — Coordinator over the three trust+identity repos (remembered
// list, pairing keys, cert pins). It re-exposes them by reference (never
// mirrored) so the TLS seam and the session manager read the SAME stores, and it
// owns the identity rules:
//
//   * machineId is the stable identity — a box re-appearing at a new IP under
//     the same machineId folds its legacy "ip:port" ghost row and carries the
//     cert pin + pairing key across, so an address change never forces a re-pair.
//   * a beacon WITHOUT a machineId refreshes an existing stable row at the same
//     address in place rather than minting an "ip:port" ghost.
//   * forget() drops the row + its pairing key + its cert pin together.

#pragma once

#include "Models/Models.h"
#include "repository/RememberedSatelliteRepository.h"
#include "repository/SatellitePinRepository.h"
#include "repository/SatelliteSharedKeyRepository.h"

#include <QList>
#include <QSettings>
#include <QString>

#include <memory>
#include <optional>

namespace dish::repository {

class ConnectionStore {
  public:
    // nullptr backs all three repos on the shared HKCU store; tests pass one
    // shared QSettings so namespace isolation and migration stay observable.
    explicit ConnectionStore(std::shared_ptr<QSettings> settings = nullptr);

    // The stable per-install device id (the X-Device-Id machineId). One owner.
    QString getOrCreateDeviceId();

    SatellitePinRepository& pins() { return *pins_; }
    SatelliteSharedKeyRepository& sharedKeys() { return *keys_; }
    RememberedSatelliteRepository& satellites() { return *satellites_; }

    QList<models::RememberedWifi> remembered() const;

    // Upsert a satellite seen by discovery / confirmed by a session, applying the
    // identity rules above. Idempotent: an identical row is not re-written.
    void rememberSatellite(const models::DiscoveredServer& server);

    // Re-point ONLY already-remembered rows from a fresh scan. Never adds an
    // un-remembered satellite; a beacon without a machineId never re-points.
    void refreshFromDiscovery(const QList<models::DiscoveredServer>& discovered);

    void forgetSatellite(const QString& id);

    std::optional<QString> satelliteSharedKey(const QString& id) const;
    void setSatelliteSharedKey(const QString& id, const QString& keyHex);
    // Drops only the pairing key, leaving the row + cert pin. Used on a terminal
    // 401 / close-notify(unpaired): the satellite stays listed as "Needs
    // pairing" and auto-retry stops.
    void forgetSatelliteSharedKey(const QString& id);

  private:
    // Refresh an existing STABLE row at the server's address in place. Returns
    // true if it handled the server.
    bool refreshKnownBox(const models::DiscoveredServer& server);
    void collapseLegacyGhosts(const models::DiscoveredServer& server, const QString& id);
    // The cert pin is keyed by IP, so it has to follow the box to a new address.
    void migratePinOnAddressChange(const std::optional<QString>& oldIp, const QString& newIp);

    std::shared_ptr<QSettings> settings_;
    std::unique_ptr<RememberedSatelliteRepository> satellites_;
    std::unique_ptr<SatelliteSharedKeyRepository> keys_;
    std::unique_ptr<SatellitePinRepository> pins_;
};

} // namespace dish::repository
