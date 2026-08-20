// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SatelliteSharedKeyRepository — per-satellite pairing-key store, under the
// "satellite_shared_key:<id>" namespace of the shared connection-store
// QSettings. The session manager reads the key to derive a session key.
//
// The key is stored as PLAINTEXT HEX in the user's registry hive, with no DPAPI
// wrap. That is a documented, deliberate trade-off — see PRIVACY.md and
// SECURITY.md before changing it.
//
// Co-tenants the cert-pin repo and remembered list in one QSettings file, kept
// disjoint by key prefix, so all()/clear() must stay prefix-scoped: a shared key
// must never leak into the pin namespace.

#pragma once

#include "architecture/Repository.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

class SatelliteSharedKeyRepository : public arch::Repository<QString, QString> {
  public:
    explicit SatelliteSharedKeyRepository(std::shared_ptr<QSettings> settings = nullptr);

    std::optional<QString> get(const QString& id) const override;
    std::vector<QString> all() const override;
    void put(const QString& id, const QString& keyHex) override;
    void remove(const QString& id) override;
    void clear() override;

  private:
    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
