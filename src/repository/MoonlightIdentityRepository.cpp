// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/MoonlightIdentityRepository.h"

#include "Util/Hex.h"
#include "repository/AppSettings.h"
#include "repository/SettingsKeys.h"

#include <array>
#include <cstdint>

namespace dish::repository {
namespace {

std::optional<QString> generateUniqueId() {
    std::array<std::uint8_t, 8> raw{};
    if (!mooncrypto::randomBytes(raw.data(), raw.size())) { return std::nullopt; }
    return QString::fromStdString(util::toHex(raw.data(), raw.size()));
}

} // namespace

MoonlightIdentityRepository::MoonlightIdentityRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings ? std::move(settings) : repository::makeSettings()) {}

std::optional<MoonlightIdentityRepository::Identity> MoonlightIdentityRepository::identity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Identity id;
    id.certPem = settings_->value(QLatin1String(keys::kMoonlightCertKey)).toString();
    id.privateKeyPem = settings_->value(QLatin1String(keys::kMoonlightKeyKey)).toString();
    id.uniqueId = settings_->value(QLatin1String(keys::kMoonlightUniqueIdKey)).toString();
    if (id.certPem.isEmpty() || id.privateKeyPem.isEmpty() || id.uniqueId.isEmpty()) {
        return std::nullopt;
    }
    if (!mooncrypto::isValidCertPem(id.certPem.toStdString())) { return std::nullopt; }
    return id;
}

std::optional<MoonlightIdentityRepository::Identity> MoonlightIdentityRepository::ensureIdentity() {
    if (auto existing = identity()) { return existing; }

    const auto generated = mooncrypto::generateClientIdentity();
    const auto uniqueId = generateUniqueId();
    if (!generated || !uniqueId) { return std::nullopt; }

    Identity id;
    id.certPem = QString::fromStdString(generated->certPem);
    id.privateKeyPem = QString::fromStdString(generated->privateKeyPem);
    id.uniqueId = *uniqueId;

    std::lock_guard<std::mutex> lock(mutex_);
    settings_->setValue(QLatin1String(keys::kMoonlightCertKey), id.certPem);
    settings_->setValue(QLatin1String(keys::kMoonlightKeyKey), id.privateKeyPem);
    settings_->setValue(QLatin1String(keys::kMoonlightUniqueIdKey), id.uniqueId);
    settings_->sync();
    return id;
}

void MoonlightIdentityRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(keys::kMoonlightCertKey));
    settings_->remove(QLatin1String(keys::kMoonlightKeyKey));
    settings_->remove(QLatin1String(keys::kMoonlightUniqueIdKey));
}

} // namespace dish::repository
