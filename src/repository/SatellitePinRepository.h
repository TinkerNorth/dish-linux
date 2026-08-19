// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SatellitePinRepository — per-satellite TLS cert-pin store (the TOFU anchor).
//
// The pinned SHA-256 fingerprint per satellite id, under the
// "satellite_cert_pin:<id>" namespace of the shared connection-store QSettings
// (HKCU\Software\Dish\Dish). The TLS callback in source/http consults it via
// core/net/Tofu; ConnectionStore migrates pins across address changes.
//
// Namespace isolation is a hard invariant: a pin must never surface as a shared
// key or vice versa. The two repos co-tenant one QSettings file and are kept
// disjoint purely by key prefix, so all()/clear() must stay prefix-scoped.

#pragma once

#include "architecture/Repository.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

class SatellitePinRepository : public arch::Repository<QString, QString> {
  public:
    // Production passes the shared connection-store QSettings so the pin/key/list
    // repos co-tenant one backing file. nullptr → the default HKCU store.
    explicit SatellitePinRepository(std::shared_ptr<QSettings> settings = nullptr);

    std::optional<QString> pinnedFingerprint(const QString& id) const { return get(id); }
    void pin(const QString& id, const QString& fingerprintHex) { put(id, fingerprintHex); }
    void forget(const QString& id) { remove(id); }

    std::optional<QString> get(const QString& id) const override;
    std::vector<QString> all() const override;
    void put(const QString& id, const QString& fingerprintHex) override;
    void remove(const QString& id) override;
    void clear() override;

  private:
    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
