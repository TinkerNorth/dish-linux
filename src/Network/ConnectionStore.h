// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "repository/ConnectionStore.h"

#include <QList>
#include <QSettings>
#include <QString>

#include <memory>
#include <optional>

namespace dish::net {

// Compatibility adapter over repository::ConnectionStore, which is the source of
// truth. It only spares the session layer a mechanical rename; new code should
// depend on the facade directly and reach the cert-pin store via facade().pins().
class ConnectionStore {
  public:
    explicit ConnectionStore(std::unique_ptr<QSettings> settings = nullptr);

    QString getOrCreateDeviceId() { return facade_->getOrCreateDeviceId(); }

    QList<models::RememberedWifi> remembered() const { return facade_->remembered(); }
    void remember(const models::DiscoveredServer& server) { facade_->rememberSatellite(server); }
    // Re-points already-remembered rows only, never adds one. Called on every
    // discovery completion so a satellite that moved to a new DHCP lease relearns
    // its persisted IP without needing a successful session first.
    void refreshFromDiscovery(const QList<models::DiscoveredServer>& discovered) {
        facade_->refreshFromDiscovery(discovered);
    }
    void forget(const QString& id) { facade_->forgetSatellite(id); }

    std::optional<QString> sharedKey(const QString& id) const {
        return facade_->satelliteSharedKey(id);
    }
    void setSharedKey(const QString& keyHex, const QString& id) {
        facade_->setSatelliteSharedKey(id, keyHex);
    }
    void forgetKey(const QString& id) { facade_->forgetSatelliteSharedKey(id); }

    repository::ConnectionStore& facade() { return *facade_; }

  private:
    std::unique_ptr<repository::ConnectionStore> facade_;
};

} // namespace dish::net
