// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The per-VID:PID raw-joystick button/axis remap behind the "Configure controls"
// page: a user-correctable routing table that overrides the best-guess
// DirectInput layout in Input/JoystickMapping for a generic pad whose buttons
// are scrambled.
//
// A corrupt blob falls back to empty and a partially-garbled entry falls back to
// that key's default remap, so an unrecognised field from a newer build is
// ignored rather than fatal. remapFor returns nullopt when nothing is stored and
// the caller uses the default.
//
// The "joystick_remaps" key namespace is disjoint from the path, deadzone and
// motion namespaces, which is what keeps clear() from touching another store.

#pragma once

#include "Input/JoystickMapping.h"

#include "architecture/Repository.h"
#include "architecture/StateSource.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QString>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace dish::source {

// One persisted per-model remap. `key` is the "%04x:%04x" vid:pid string.
struct JoystickRemapEntry {
    QString key;
    input::JoystickRemap remap;

    bool operator==(const JoystickRemapEntry& o) const { return key == o.key && remap == o.remap; }
    bool operator!=(const JoystickRemapEntry& o) const { return !(*this == o); }
};

// The "%04x:%04x" storage key for a (vid, pid). Same shape as usbPathKeyFor but
// kept distinct so a future key-format change to one store cannot silently move
// the other.
QString joystickRemapKeyFor(int vendorId, int productId);

class JoystickRemapRepository : public arch::KeyedRepository<QString, JoystickRemapEntry> {
  public:
    // `settings` lets tests inject an isolated store; production passes the
    // default HKCU app store. nullptr -> the default store.
    explicit JoystickRemapRepository(std::shared_ptr<QSettings> settings = nullptr);

    QString keyOf(const JoystickRemapEntry& value) const override { return value.key; }

    std::optional<JoystickRemapEntry> get(const QString& key) const override;
    std::vector<JoystickRemapEntry> all() const override;
    void put(const QString& key, const JoystickRemapEntry& value) override;
    void remove(const QString& key) override;
    void clear() override;

    using arch::KeyedRepository<QString, JoystickRemapEntry>::put;
    using arch::KeyedRepository<QString, JoystickRemapEntry>::removeValue;

    // The QSettings key the whole remaps object is persisted under (exposed so
    // the store test can seed a corrupt / forward-incompatible blob). MUST stay
    // disjoint from UsbPathPreferenceRepository::kChoicesKey.
    static constexpr const char* kRemapsKey = "joystick_remaps";

  private:
    // Read/write the whole storageKey->entry map. Both assume mutex_ is held.
    std::map<QString, JoystickRemapEntry> readMap() const;
    void writeMap(const std::map<QString, JoystickRemapEntry>& map);

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

// slotKey ("%04x:%04x") -> JoystickRemap. std::map for a deterministic,
// ==-comparable Observable value so distinct-until-changed suppresses no-ops.
using JoystickRemapMap = std::map<std::string, input::JoystickRemap>;

class JoystickRemapStore : public arch::StateSource<JoystickRemapMap> {
  public:
    // `repo` is the durable backing store; read once at construction to hydrate
    // and written through on every mutation. Borrowed, not owned.
    explicit JoystickRemapStore(JoystickRemapRepository* repo);

    // The stored remap for a model, or std::nullopt (== use the default layout)
    // if never written.
    std::optional<input::JoystickRemap> remapFor(int vendorId, int productId) const;

    // Persist + republish the remap. An unchanged write short-circuits (no emit).
    void setRemap(int vendorId, int productId, const input::JoystickRemap& remap);

    // Drop the model's remap from both the repo and the live state. A no-op (no
    // emit) on the state side if it was absent.
    void clearRemap(int vendorId, int productId);

  private:
    static JoystickRemapMap hydrate(JoystickRemapRepository* repo);

    JoystickRemapRepository* repo_;
};

// ── JSON (exposed for the repository test's corrupt/forward-compat seeding) ──

// Serialize a JoystickRemap to a JSON object. Total — every field is emitted.
QJsonObject joystickRemapToJson(const input::JoystickRemap& remap);

// Deserialize a JoystickRemap from a JSON value. Unknown / missing / wrong-typed
// fields fall back to the DEFAULT remap's value for that field (forward-compat),
// so a partially-garbled or newer-build blob still yields a usable remap rather
// than crashing.
input::JoystickRemap joystickRemapFromJson(const QJsonValue& v);

} // namespace dish::source
