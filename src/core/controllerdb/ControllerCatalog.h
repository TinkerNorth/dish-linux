// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One whole published database as the app reads it: the records, an immutable
// index from a USB or Bluetooth id to its record, and the strings by locale
// with the `en` fallback (docs/controller-db.md sections 5 and 6.2). Built once
// per load and shared read-only; a later dbVersion replaces the whole catalog,
// never one record at a time.

#pragma once

#include "core/controllerdb/ControllerRecord.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dish::controllerdb {

enum class DbSource : std::uint8_t { Bundled, Cache };

inline constexpr std::string_view kFallbackLocale = "en";

// The six locales the strings files come in, in the manifest's order.
inline constexpr std::array<std::string_view, 6> kStringsLocales{"en", "bs", "de",
                                                                 "es", "fr", "pt-BR"};

class ControllerCatalog {
  public:
    ControllerCatalog() = default;
    ControllerCatalog(ControllerDb db, std::map<std::string, StringsTable> strings,
                      DbSource source);

    int dbVersion() const { return db_.dbVersion; }
    DbSource source() const { return source_; }
    const ControllerDb& db() const { return db_; }

    // The first record that lists the id, or null. A pad that shows two records the same id is a
    // data bug the database's own tests refuse; first wins so the answer is at least stable.
    const ControllerRecord* findUsb(DeviceId id) const;
    const ControllerRecord* findBluetooth(DeviceId id) const;
    const ControllerRecord* findById(std::string_view recordId) const;

    // The locale's text for a key, else `en`'s, else empty.
    std::string text(std::string_view key, std::string_view locale) const;

  private:
    using Index = std::unordered_map<std::uint32_t, std::size_t>;
    static Index indexOf(const std::vector<ControllerRecord>& records,
                         const std::vector<DeviceId> ControllerRecord::* ids);

    ControllerDb db_;
    std::map<std::string, StringsTable> strings_;
    DbSource source_ = DbSource::Bundled;
    Index usb_;
    Index bluetooth_;
};

using CatalogPtr = std::shared_ptr<const ControllerCatalog>;

// Bundled first, then the cached download if its dbVersion is greater (section 6.2).
CatalogPtr chooseActive(const CatalogPtr& bundled, const CatalogPtr& cached);

// The strings locale for the app's UI languages, most preferred first ("de-DE", "pt-BR",
// "bs-Latn-BA"): the first one the database has a file for, else `en`.
std::string stringsLocaleFor(const std::vector<std::string>& uiLanguages);

} // namespace dish::controllerdb
