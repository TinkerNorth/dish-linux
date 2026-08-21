// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SatelliteSharedKeyRepository — per-satellite pairing-key store, under the
// "satellite_shared_key:<id>" namespace of the shared connection-store
// QSettings. The session manager reads the key to derive a session key.
//
// Keys live in the desktop keyring when one answers on the session bus, and in
// the config file when none does. `secrets` is injected, never constructed here:
// a repository that reached for the real keyring on its own would make every test
// that builds one write to the developer's own login keyring.
//
// The config-file path stores PLAINTEXT HEX, so AppSettings keeps that file 0600.
// A key found there while a keyring IS available is migrated on first read and
// then deleted from the file — see get().
//
// Co-tenants the cert-pin repo and remembered list in one QSettings file, kept
// disjoint by key prefix, so all()/clear() must stay prefix-scoped: a shared key
// must never leak into the pin namespace.

#pragma once

#include "architecture/Repository.h"
#include "source/system/SecretServiceStore.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

class SatelliteSharedKeyRepository : public arch::Repository<QString, QString> {
  public:
    explicit SatelliteSharedKeyRepository(
        std::shared_ptr<QSettings> settings = nullptr,
        std::shared_ptr<source::SecretServiceStore> secrets = nullptr);

    std::optional<QString> get(const QString& id) const override;
    std::vector<QString> all() const override;
    void put(const QString& id, const QString& keyHex) override;
    void remove(const QString& id) override;
    void clear() override;

  private:
    // True only when a keyring answered AND opened a session. Everything below
    // branches on this rather than on the pointer, so an unusable service behaves
    // exactly like an absent one.
    bool keyringUsable() const;

    std::shared_ptr<QSettings> settings_;
    std::shared_ptr<source::SecretServiceStore> secrets_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
