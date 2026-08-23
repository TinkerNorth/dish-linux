// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/SatelliteSharedKeyRepository.h"

#include "repository/AppSettings.h"
#include "repository/SettingsKeys.h"

#include <QStringList>

#include <algorithm>

namespace dish::repository {

SatelliteSharedKeyRepository::SatelliteSharedKeyRepository(
    std::shared_ptr<QSettings> settings, std::shared_ptr<source::SecretServiceStore> secrets)
    : settings_(settings ? std::move(settings) : repository::makeSettings()),
      secrets_(std::move(secrets)) {}

bool SatelliteSharedKeyRepository::keyringUsable() const {
    return secrets_ != nullptr && secrets_->available();
}

std::optional<QString> SatelliteSharedKeyRepository::get(const QString& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const QString settingsKey = QLatin1String(keys::kSharedKeyPrefix) + id;

    if (keyringUsable()) {
        if (auto fromKeyring = secrets_->read(id)) {
            // The keyring is authoritative once it holds the key, so any copy
            // still in the config file is stale plaintext. Sweeping it here is
            // what makes the migration actually finish: without it, a file copy
            // survives forever behind a successful keyring read, and the secret
            // never leaves the disk.
            const QString stale = settings_->value(settingsKey).toString();
            if (!stale.isEmpty()) {
                settings_->remove(settingsKey);
                settings_->sync();
            }
            return fromKeyring;
        }

        // Not in the keyring yet. A key still in the config file is a pre-keyring
        // install, so move it across on the way past: the alternative is leaving
        // the plaintext copy behind forever, since nothing else would ever
        // rewrite a key that already works.
        auto legacy = settings_->value(settingsKey).toString();
        if (legacy.isEmpty()) { return std::nullopt; }
        if (secrets_->write(id, legacy)) {
            settings_->remove(settingsKey);
            settings_->sync();
        }
        // Returned either way: a keyring that refused the write must not cost the
        // user their session, and the file copy is still perfectly valid.
        return legacy;
    }

    // Non-const on purpose: const blocks the implicit move into the returned
    // optional and costs a QString copy on every hit.
    auto v = settings_->value(settingsKey).toString();
    if (v.isEmpty()) { return std::nullopt; }
    return v;
}

std::vector<QString> SatelliteSharedKeyRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QString> out;

    if (keyringUsable()) {
        for (const auto& id : secrets_->ids()) {
            if (auto value = secrets_->read(id)) { out.push_back(*value); }
        }
    }

    // The file is still read even with a keyring present: until every id has been
    // through get() once, some keys are in one store and some in the other, and a
    // caller counting keys must not see the set shrink mid-migration.
    const QString prefix = QLatin1String(keys::kSharedKeyPrefix);
    for (const auto& key : settings_->allKeys()) {
        if (!key.startsWith(prefix)) { continue; }
        const auto v = settings_->value(key).toString();
        if (v.isEmpty()) { continue; }
        if (std::find(out.begin(), out.end(), v) == out.end()) { out.push_back(v); }
    }
    return out;
}

void SatelliteSharedKeyRepository::put(const QString& id, const QString& keyHex) {
    std::lock_guard<std::mutex> lock(mutex_);
    const QString settingsKey = QLatin1String(keys::kSharedKeyPrefix) + id;

    if (keyringUsable() && secrets_->write(id, keyHex)) {
        // A re-pair over a pre-keyring install would otherwise leave the OLD key
        // in the file, where the fallback path would later read it back.
        settings_->remove(settingsKey);
        settings_->sync();
        return;
    }

    // Plaintext hex in a 0600 file — the documented fallback. See the header.
    settings_->setValue(settingsKey, keyHex);
    restrictSettingsPermissions();
}

void SatelliteSharedKeyRepository::remove(const QString& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Both stores, unconditionally: a forget must not leave a copy anywhere that
    // a later read could resurrect.
    if (keyringUsable()) { secrets_->erase(id); }
    settings_->remove(QLatin1String(keys::kSharedKeyPrefix) + id);
}

void SatelliteSharedKeyRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (keyringUsable()) {
        for (const auto& id : secrets_->ids()) { secrets_->erase(id); }
    }
    const QString prefix = QLatin1String(keys::kSharedKeyPrefix);
    QStringList toRemove;
    for (const auto& key : settings_->allKeys()) {
        if (key.startsWith(prefix)) { toRemove.append(key); }
    }
    for (const auto& key : toRemove) { settings_->remove(key); }
}

} // namespace dish::repository
