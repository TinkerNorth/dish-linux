// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/controllerdb/ControllerCatalog.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>

namespace dish::controllerdb {

namespace {

const ControllerRecord* recordAt(const std::vector<ControllerRecord>& records,
                                 const std::unordered_map<std::uint32_t, std::size_t>& index,
                                 DeviceId id) {
    const auto it = index.find(id.key());
    return it == index.end() ? nullptr : &records[it->second];
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

// "pt-BR" is the one locale named with its region; every other one is the bare language.
std::string_view supportedLocaleOf(std::string_view uiLanguage) {
    const std::string lower = lowered(uiLanguage);
    const std::size_t cut = lower.find_first_of("-_");
    const std::string language = lower.substr(0, cut);
    if (language == "pt") { return "pt-BR"; }
    for (const std::string_view locale : kStringsLocales) {
        if (locale == language) { return locale; }
    }
    return {};
}

} // namespace

ControllerCatalog::ControllerCatalog(ControllerDb db, std::map<std::string, StringsTable> strings,
                                     DbSource source)
    : db_(std::move(db)), strings_(std::move(strings)), source_(source),
      usb_(indexOf(db_.records, &ControllerRecord::usb)),
      bluetooth_(indexOf(db_.records, &ControllerRecord::bluetooth)) {}

ControllerCatalog::Index
ControllerCatalog::indexOf(const std::vector<ControllerRecord>& records,
                           const std::vector<DeviceId> ControllerRecord::* ids) {
    Index index;
    for (std::size_t i = 0; i < records.size(); ++i) {
        for (const DeviceId& id : records[i].*ids) { index.emplace(id.key(), i); }
    }
    return index;
}

const ControllerRecord* ControllerCatalog::findUsb(DeviceId id) const {
    return recordAt(db_.records, usb_, id);
}

const ControllerRecord* ControllerCatalog::findBluetooth(DeviceId id) const {
    return recordAt(db_.records, bluetooth_, id);
}

const ControllerRecord* ControllerCatalog::findById(std::string_view recordId) const {
    const auto it =
        std::find_if(db_.records.begin(), db_.records.end(),
                     [recordId](const ControllerRecord& r) { return r.id == recordId; });
    return it == db_.records.end() ? nullptr : &*it;
}

std::string ControllerCatalog::text(std::string_view key, std::string_view locale) const {
    const std::string wanted(key);
    for (const std::string_view candidate : {locale, kFallbackLocale}) {
        const auto table = strings_.find(std::string(candidate));
        if (table == strings_.end()) { continue; }
        const auto entry = table->second.find(wanted);
        if (entry != table->second.end()) { return entry->second; }
    }
    return {};
}

CatalogPtr chooseActive(const CatalogPtr& bundled, const CatalogPtr& cached) {
    const bool cacheIsNewer =
        cached != nullptr && (bundled == nullptr || cached->dbVersion() > bundled->dbVersion());
    return cacheIsNewer ? cached : bundled;
}

std::string stringsLocaleFor(const std::vector<std::string>& uiLanguages) {
    for (const std::string& language : uiLanguages) {
        const std::string_view locale = supportedLocaleOf(language);
        if (!locale.empty()) { return std::string(locale); }
    }
    return std::string(kFallbackLocale);
}

} // namespace dish::controllerdb
