// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/SatellitePinRepository.h"

#include "repository/SettingsKeys.h"

namespace dish::repository {

SatellitePinRepository::SatellitePinRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {
}

std::optional<QString> SatellitePinRepository::get(const QString& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Non-const on purpose: const blocks the implicit move into the returned
    // optional and costs a QString copy on every hit.
    auto v = settings_->value(QLatin1String(keys::kCertPinPrefix) + id).toString();
    if (v.isEmpty()) { return std::nullopt; }
    return v;
}

std::vector<QString> SatellitePinRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QString> out;
    const QString prefix = QLatin1String(keys::kCertPinPrefix);
    for (const auto& key : settings_->allKeys()) {
        if (key.startsWith(prefix)) {
            const auto v = settings_->value(key).toString();
            if (!v.isEmpty()) { out.push_back(v); }
        }
    }
    return out;
}

void SatellitePinRepository::put(const QString& id, const QString& fingerprintHex) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->setValue(QLatin1String(keys::kCertPinPrefix) + id, fingerprintHex);
}

void SatellitePinRepository::remove(const QString& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(keys::kCertPinPrefix) + id);
}

void SatellitePinRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const QString prefix = QLatin1String(keys::kCertPinPrefix);
    // Only this repo's namespace: co-tenant shared keys and the remembered list
    // must survive. Collect first — removing while iterating allKeys() is unsafe.
    QStringList toRemove;
    for (const auto& key : settings_->allKeys()) {
        if (key.startsWith(prefix)) { toRemove.append(key); }
    }
    for (const auto& key : toRemove) { settings_->remove(key); }
}

} // namespace dish::repository
