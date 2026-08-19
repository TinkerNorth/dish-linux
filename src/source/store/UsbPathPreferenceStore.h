// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The per-VID:PID USB input-path preference, as a durable Repository wrapped in
// a reactive Source.
//
// A corrupt blob falls back to empty and an unrecognised stored value (one a
// newer build wrote) is dropped on read, so a downgrade never crashes and never
// guesses.
//
// The vid:pid key namespace is disjoint from the trust, deadzone and motion
// namespaces, which is what keeps clear() from touching another store's data.

#pragma once

#include "core/reducer/PathChoice.h"

#include "architecture/Repository.h"
#include "architecture/StateSource.h"

#include <QSettings>
#include <QString>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace dish::source {

// One persisted per-model path choice. `key` is the "%04x:%04x" vid:pid string.
struct UsbPathEntry {
    QString key;
    reducer::PathChoice choice = reducer::PathChoice::Standard;

    bool operator==(const UsbPathEntry& o) const { return key == o.key && choice == o.choice; }
    bool operator!=(const UsbPathEntry& o) const { return !(*this == o); }
};

// The "%04x:%04x" storage key for a (vid, pid). Mirrors android keyFor().
QString usbPathKeyFor(int vendorId, int productId);

class UsbPathPreferenceRepository : public arch::KeyedRepository<QString, UsbPathEntry> {
  public:
    // `settings` lets tests inject an isolated in-memory-style store; production
    // passes the default HKCU app store. nullptr -> the default store.
    explicit UsbPathPreferenceRepository(std::shared_ptr<QSettings> settings = nullptr);

    QString keyOf(const UsbPathEntry& value) const override { return value.key; }

    std::optional<UsbPathEntry> get(const QString& key) const override;
    std::vector<UsbPathEntry> all() const override;
    void put(const QString& key, const UsbPathEntry& value) override;
    void remove(const QString& key) override;
    void clear() override;

    using arch::KeyedRepository<QString, UsbPathEntry>::put;
    using arch::KeyedRepository<QString, UsbPathEntry>::removeValue;

    // The QSettings key the whole choices object is persisted under (exposed so
    // the store test can seed a corrupt / forward-incompatible blob).
    static constexpr const char* kChoicesKey = "usb_path_choices";

  private:
    // Read/write the whole storageKey->entry map (the entry preserves its own
    // key verbatim). Both assume mutex_ is held.
    std::map<QString, UsbPathEntry> readMap() const;
    void writeMap(const std::map<QString, UsbPathEntry>& map);

    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

// slotKey ("%04x:%04x") -> PathChoice. std::map for a deterministic,
// ==-comparable Observable value so distinct-until-changed suppresses no-ops.
using UsbPathChoiceMap = std::map<std::string, reducer::PathChoice>;

class UsbPathPreferenceStore : public arch::StateSource<UsbPathChoiceMap> {
  public:
    // `repo` is the durable backing store; read once at construction to hydrate
    // and written through on every mutation. Borrowed, not owned.
    explicit UsbPathPreferenceStore(UsbPathPreferenceRepository* repo);

    // The stored pick for a model, or std::nullopt (== Auto) if never written.
    std::optional<reducer::PathChoice> choiceFor(int vendorId, int productId) const;

    // Persist + republish the pick. An unchanged write short-circuits (no emit).
    void setChoice(int vendorId, int productId, reducer::PathChoice choice);

    // Drop the model's pick from both the repo and the live state. A no-op (no
    // emit) on the state side if it was absent.
    void clearChoice(int vendorId, int productId);

  private:
    static UsbPathChoiceMap hydrate(UsbPathPreferenceRepository* repo);

    UsbPathPreferenceRepository* repo_;
};

} // namespace dish::source
