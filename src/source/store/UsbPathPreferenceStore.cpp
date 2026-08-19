// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/UsbPathPreferenceStore.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace dish::source {

QString usbPathKeyFor(int vendorId, int productId) {
    return QStringLiteral("%1:%2")
        .arg(static_cast<unsigned>(vendorId) & 0xFFFFu, 4, 16, QLatin1Char('0'))
        .arg(static_cast<unsigned>(productId) & 0xFFFFu, 4, 16, QLatin1Char('0'));
}

// ── Repository ──────────────────────────────────────────────────────────────

UsbPathPreferenceRepository::UsbPathPreferenceRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {
}

namespace {

// Field names for the per-entry object form. The entry's OWN key (`vk`) is
// stored verbatim independent of the storage key, so get(key)/put(key, value)
// round-trip the value exactly even when value.key != key — the Map<K,V>
// faithfulness RepositoryContract pins (the sibling DeadzoneRepository /
// MotionPreferenceRepository keep the same invariant). In normal use value.key
// == the storage key, so the two coincide.
constexpr const char* kFieldKey = "vk";
constexpr const char* kFieldChoice = "c";

// Decode one persisted value. Two accepted shapes:
//   * a bare choice STRING ("direct"/"standard") — the android-compatible /
//     legacy / forward form. The entry's key is then the storage key.
//   * an object {vk:<key>, c:<choice>} — the faithful form put() writes.
// Returns nullopt for an unknown/garbled value (dropped on read for
// forward-compat).
std::optional<UsbPathEntry> decodeValue(const QString& storageKey, const QJsonValue& v) {
    if (v.isString()) {
        if (auto choice = reducer::pathChoiceFromStorageValue(v.toString().toStdString())) {
            return UsbPathEntry{storageKey, *choice};
        }
        return std::nullopt;
    }
    if (v.isObject()) {
        const auto obj = v.toObject();
        const auto choice = reducer::pathChoiceFromStorageValue(
            obj.value(QLatin1String(kFieldChoice)).toString().toStdString());
        if (!choice.has_value()) { return std::nullopt; }
        const QString vk = obj.contains(QLatin1String(kFieldKey))
                               ? obj.value(QLatin1String(kFieldKey)).toString()
                               : storageKey;
        return UsbPathEntry{vk, *choice};
    }
    return std::nullopt;
}

} // namespace

std::map<QString, UsbPathEntry> UsbPathPreferenceRepository::readMap() const {
    const auto raw = settings_->value(QLatin1String(kChoicesKey)).toByteArray();
    if (raw.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(raw, &err);
    // Corrupt blob / wrong shape -> empty (don't crash on garbled prefs).
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return {}; }

    std::map<QString, UsbPathEntry> out;
    const auto obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (auto entry = decodeValue(it.key(), it.value())) { out.emplace(it.key(), *entry); }
    }
    return out;
}

void UsbPathPreferenceRepository::writeMap(const std::map<QString, UsbPathEntry>& map) {
    QJsonObject obj;
    for (const auto& [key, entry] : map) {
        obj.insert(key, QJsonObject{{kFieldKey, entry.key},
                                    {kFieldChoice, QString::fromStdString(
                                                       reducer::toStorageValue(entry.choice))}});
    }
    settings_->setValue(QLatin1String(kChoicesKey),
                        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

std::optional<UsbPathEntry> UsbPathPreferenceRepository::get(const QString& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto map = readMap();
    const auto it = map.find(key);
    if (it == map.end()) { return std::nullopt; }
    return it->second;
}

std::vector<UsbPathEntry> UsbPathPreferenceRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UsbPathEntry> out;
    for (const auto& [key, entry] : readMap()) { out.push_back(entry); }
    return out;
}

void UsbPathPreferenceRepository::put(const QString& key, const UsbPathEntry& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto map = readMap();
    // Storage key authoritative; store the value verbatim under it.
    map[key] = value;
    writeMap(map);
}

void UsbPathPreferenceRepository::remove(const QString& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto map = readMap();
    map.erase(key);
    writeMap(map);
}

void UsbPathPreferenceRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(kChoicesKey));
}

// ── StateSource ───────────────────────────────────────────────────────────--

UsbPathChoiceMap UsbPathPreferenceStore::hydrate(UsbPathPreferenceRepository* repo) {
    UsbPathChoiceMap out;
    if (repo == nullptr) { return out; }
    for (const auto& e : repo->all()) { out[e.key.toStdString()] = e.choice; }
    return out;
}

UsbPathPreferenceStore::UsbPathPreferenceStore(UsbPathPreferenceRepository* repo)
    : arch::StateSource<UsbPathChoiceMap>(hydrate(repo)), repo_(repo) {}

std::optional<reducer::PathChoice> UsbPathPreferenceStore::choiceFor(int vendorId,
                                                                     int productId) const {
    const auto& snapshot = state().value();
    const auto it = snapshot.find(usbPathKeyFor(vendorId, productId).toStdString());
    if (it == snapshot.end()) { return std::nullopt; }
    return it->second;
}

void UsbPathPreferenceStore::setChoice(int vendorId, int productId, reducer::PathChoice choice) {
    const std::string key = usbPathKeyFor(vendorId, productId).toStdString();
    // Short-circuit an unchanged write so the Observable does not re-emit.
    const auto& current = state().value();
    const auto it = current.find(key);
    if (it != current.end() && it->second == choice) { return; }

    if (repo_ != nullptr) { repo_->put(UsbPathEntry{usbPathKeyFor(vendorId, productId), choice}); }
    setState([&](const UsbPathChoiceMap& cur) {
        UsbPathChoiceMap next = cur;
        next[key] = choice;
        return next;
    });
}

void UsbPathPreferenceStore::clearChoice(int vendorId, int productId) {
    const std::string key = usbPathKeyFor(vendorId, productId).toStdString();
    if (repo_ != nullptr) { repo_->remove(usbPathKeyFor(vendorId, productId)); }
    setState([&](const UsbPathChoiceMap& cur) {
        if (cur.find(key) == cur.end()) { return cur; }
        UsbPathChoiceMap next = cur;
        next.erase(key);
        return next;
    });
}

} // namespace dish::source
