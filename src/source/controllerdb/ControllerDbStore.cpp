// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/controllerdb/ControllerDbStore.h"

#include "core/controllerdb/DbFetch.h"
#include "source/controllerdb/ControllerDbJson.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// Outside the namespace, as Q_INIT_RESOURCE requires. dish_core is a static library, so nothing
// else forces the linker to keep the resource object.
static void initBundledDbResource() {
    static std::once_flag once;
    std::call_once(once, [] { Q_INIT_RESOURCE(controllerdb); });
}

namespace dish::source::controllerdb {

namespace {

Q_LOGGING_CATEGORY(lcDishControllerDb, "dish.controllerdb")

constexpr const char* kCurrentFile = "current";
constexpr const char* kStringsPrefix = "strings/";
constexpr const char* kJsonSuffix = ".json";

// A path or a token of the core's, as Qt text.
QString pathOf(std::string_view path) {
    return QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
}

QByteArray readWhole(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    return file.readAll();
}

bool writeWhole(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { return false; }
    const bool wrote = file.write(bytes) == bytes.size();
    return wrote && file.commit();
}

std::string sha256Hex(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

// "strings/pt-BR.json" is the pt-BR table; anything else is no strings file.
std::optional<std::string> stringsLocaleOf(const std::string& path) {
    const std::string prefix = kStringsPrefix;
    const std::string suffix = kJsonSuffix;
    const bool shaped = path.size() > prefix.size() + suffix.size() &&
                        path.compare(0, prefix.size(), prefix) == 0 &&
                        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    if (!shaped) { return std::nullopt; }
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

// Each promised file at its promised size and digest.
bool everyFileAsPromised(const cdb::DbManifest& manifest, const DbFiles& files) {
    for (const cdb::ManifestFile& promised : manifest.files) {
        const auto it = files.files.find(pathOf(promised.path));
        if (it == files.files.end()) { return false; }
        const QByteArray& bytes = it->second;
        if (!cdb::fileMatches(promised, sha256Hex(bytes), bytes.size())) { return false; }
    }
    return true;
}

std::optional<std::map<std::string, cdb::StringsTable>> stringsOf(const cdb::DbManifest& manifest,
                                                                  const DbFiles& files) {
    std::map<std::string, cdb::StringsTable> out;
    for (const cdb::ManifestFile& promised : manifest.files) {
        const std::optional<std::string> locale = stringsLocaleOf(promised.path);
        if (!locale.has_value()) { continue; }
        auto parsed = parseStringsTable(files.files.at(pathOf(promised.path)));
        auto* table = std::get_if<cdb::StringsTable>(&parsed);
        if (table == nullptr) { return std::nullopt; }
        out.emplace(*locale, std::move(*table));
    }
    return out;
}

} // namespace

QString controllerDbCacheRoot() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) { return {}; }
    return base + QStringLiteral("/controllers/v1");
}

DbFiles readDbFolder(const QString& dir) {
    DbFiles out;
    out.manifest = readWhole(dir + QLatin1Char('/') + pathOf(cdb::kManifestPath));
    for (const std::string_view path : cdb::kDbFilePaths) {
        const QString full = dir + QLatin1Char('/') + pathOf(path);
        if (!QFile::exists(full)) { continue; }
        out.files.emplace(pathOf(path), readWhole(full));
    }
    return out;
}

cdb::CatalogPtr catalogFromFiles(const DbFiles& files, cdb::DbSource source) {
    auto parsedManifest = parseDbManifest(files.manifest);
    const auto* manifest = std::get_if<cdb::DbManifest>(&parsedManifest);
    if (manifest == nullptr || !cdb::manifestIsUsable(*manifest)) { return nullptr; }
    if (!everyFileAsPromised(*manifest, files)) { return nullptr; }

    auto parsedDb = parseControllerDb(files.files.at(pathOf(cdb::kRecordsPath)));
    auto* db = std::get_if<cdb::ControllerDb>(&parsedDb);
    if (db == nullptr || !cdb::recordsMatchManifest(*db, *manifest)) { return nullptr; }

    auto strings = stringsOf(*manifest, files);
    if (!strings.has_value()) { return nullptr; }
    return std::make_shared<const cdb::ControllerCatalog>(std::move(*db), std::move(*strings),
                                                          source);
}

ControllerDbStore::ControllerDbStore(source::ControllerDbPreferenceStore* prefs, QString bundledDir,
                                     QString cacheRoot)
    : arch::StateSource<ControllerDbStatus>(ControllerDbStatus{}), prefs_(prefs),
      bundledDir_(std::move(bundledDir)), cacheRoot_(std::move(cacheRoot)) {}

void ControllerDbStore::load() {
    initBundledDbResource();
    bundled_ = catalogFromFiles(readDbFolder(bundledDir_), cdb::DbSource::Bundled);
    if (bundled_ == nullptr) {
        qCWarning(lcDishControllerDb) << "the bundled controller database did not verify";
    }
    active_ = cdb::chooseActive(bundled_, loadCache());
    publish();
}

cdb::CatalogPtr ControllerDbStore::loadCache() const {
    if (cacheRoot_.isEmpty()) { return nullptr; }
    const QByteArray current =
        readWhole(cacheRoot_ + QLatin1Char('/') + QLatin1String(kCurrentFile)).trimmed();
    if (current.isEmpty()) { return nullptr; }
    const QString folder = cacheRoot_ + QLatin1Char('/') + QString::fromLatin1(current);
    return catalogFromFiles(readDbFolder(folder), cdb::DbSource::Cache);
}

// The folder first, `current` last: until that rename lands, the database in force is untouched.
bool ControllerDbStore::commit(const DbFiles& files, const cdb::CatalogPtr& catalog) {
    if (cacheRoot_.isEmpty() || catalog == nullptr) { return false; }
    const QString version = QString::number(catalog->dbVersion());
    const QString folder = cacheRoot_ + QLatin1Char('/') + version;
    if (!QDir().mkpath(folder + QStringLiteral("/strings"))) { return false; }
    if (!writeWhole(folder + QLatin1Char('/') + pathOf(cdb::kManifestPath), files.manifest)) {
        return false;
    }
    for (const auto& [path, bytes] : files.files) {
        if (!writeWhole(folder + QLatin1Char('/') + path, bytes)) { return false; }
    }
    if (!writeWhole(cacheRoot_ + QLatin1Char('/') + QLatin1String(kCurrentFile),
                    version.toLatin1())) {
        return false;
    }
    // The folders the swap retired; one that cannot be removed now is tried again at the next
    // commit.
    const QDir root(cacheRoot_);
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name != version) { QDir(root.filePath(name)).removeRecursively(); }
    }
    active_ = catalog;
    publish();
    return true;
}

void ControllerDbStore::setChecking(bool checking) {
    ControllerDbStatus next = state().value();
    next.checking = checking;
    setState(next);
}

void ControllerDbStore::recordCheck(std::int64_t atMs, cdb::CheckResult result) {
    prefs_->setLastCheck(ControllerDbLastCheck{atMs, pathOf(cdb::checkResultToken(result))});
    publish();
}

void ControllerDbStore::publish() {
    ControllerDbStatus next = state().value();
    next.bundledVersion = bundled_ != nullptr ? bundled_->dbVersion() : 0;
    next.activeVersion = active_ != nullptr ? active_->dbVersion() : 0;
    next.source = active_ != nullptr ? active_->source() : cdb::DbSource::Bundled;
    const ControllerDbLastCheck last = prefs_->lastCheck();
    next.lastCheckAtMs = last.atMs;
    next.lastCheckResult = cdb::checkResultFrom(last.result.toStdString());
    setState(next);
}

} // namespace dish::source::controllerdb
