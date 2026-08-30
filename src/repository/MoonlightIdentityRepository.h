// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MoonlightIdentityRepository — the per-install Moonlight client identity.
//
// One self-signed RSA cert + private key (PEM) plus the GameStream `uniqueid`,
// co-tenant in the shared connection-store QSettings beside the satellite
// trust material. The cert authenticates every HTTPS call to every paired
// Moonlight host, so it is generated exactly once and never rotated silently:
// losing it means the user re-pairs each host.
//
// The private key is stored in the settings file rather than the keyring
// because a 2048-bit PEM exceeds what several keyring backends accept per
// item, and the file already holds material of the same sensitivity class
// (the satellite shared-key fallback path).

#pragma once

#include "core/moonlight/MoonlightPairingCrypto.h"

#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

class MoonlightIdentityRepository {
  public:
    // Production passes the shared connection-store QSettings; nullptr → the
    // default store (tests pass a scratch file).
    explicit MoonlightIdentityRepository(std::shared_ptr<QSettings> settings = nullptr);

    struct Identity {
        QString certPem;
        QString privateKeyPem;
        QString uniqueId; // 16 lowercase hex chars
    };

    // The stored identity, or nullopt when none was ever generated (or the
    // stored blob no longer parses as a certificate).
    std::optional<Identity> identity() const;

    // Returns the stored identity, generating and persisting one on first
    // call. nullopt only when key generation itself fails.
    std::optional<Identity> ensureIdentity();

    // Drops the identity. Every paired host then requires a fresh pairing.
    void clear();

  private:
    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
