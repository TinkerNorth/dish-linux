// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// RememberedSatelliteRepository — the durable list of remembered satellites.
//
// Unlike the per-key pin/shared-key repos, the whole set lives under the single
// "satellite_list" key of the shared connection-store QSettings, so a satellite
// re-appearing is a read-modify-write of the entire list. Corrupt JSON falls
// back to an empty list; a garbled blob must not brick discovery.

#pragma once

#include "Models/Models.h"
#include "architecture/Repository.h"
#include "repository/RememberedWifiEq.h"

#include <QList>
#include <QMap>
#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

class RememberedSatelliteRepository
    : public arch::KeyedRepository<QString, models::RememberedWifi> {
  public:
    explicit RememberedSatelliteRepository(std::shared_ptr<QSettings> settings = nullptr);

    QString keyOf(const models::RememberedWifi& value) const override { return value.id; }

    std::optional<models::RememberedWifi> get(const QString& id) const override;
    std::vector<models::RememberedWifi> all() const override;
    void put(const QString& id, const models::RememberedWifi& value) override;
    void remove(const QString& id) override;
    void clear() override;

    // Un-hide the KeyedRepository value-overloads the declarations above shadow.
    using arch::KeyedRepository<QString, models::RememberedWifi>::put;
    using arch::KeyedRepository<QString, models::RememberedWifi>::removeValue;

  private:
    // Both assume mutex_ is held.
    QMap<QString, models::RememberedWifi> readEntries() const;
    void writeEntries(const QMap<QString, models::RememberedWifi>& entries);

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
