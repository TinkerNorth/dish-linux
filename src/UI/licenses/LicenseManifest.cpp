// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "UI/licenses/LicenseManifest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace dish::ui {

namespace {

// A present-but-blank value behaves like absent for display.
std::optional<QString> optString(const QJsonObject& obj, const char* key) {
    const QJsonValue v = obj.value(QLatin1String(key));
    if (!v.isString()) { return std::nullopt; }
    const QString s = v.toString();
    if (s.isEmpty()) { return std::nullopt; }
    return s;
}

LicenseInfo parseLicenseInfo(const QJsonObject& obj) {
    LicenseInfo info;
    info.name = optString(obj, "name");
    info.url = optString(obj, "url");
    return info;
}

LicenseEntry parseLicenseEntry(const QJsonObject& obj) {
    LicenseEntry entry;
    entry.group = optString(obj, "group");
    entry.artifact = optString(obj, "artifact");
    entry.version = optString(obj, "version");
    entry.name = optString(obj, "name");
    entry.url = optString(obj, "url");
    const QJsonValue licenses = obj.value(QLatin1String("licenses"));
    if (licenses.isArray()) {
        for (const QJsonValue& v : licenses.toArray()) {
            if (v.isObject()) { entry.licenses.push_back(parseLicenseInfo(v.toObject())); }
        }
    }
    return entry;
}

bool nonBlank(const std::optional<QString>& s) { return s.has_value() && !s->trimmed().isEmpty(); }

} // namespace

LicenseManifest parseLicenseManifest(const QByteArray& json) {
    LicenseManifest manifest;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return manifest; }
    const QJsonObject root = doc.object();
    manifest.generatedBy = optString(root, "generatedBy");
    const QJsonValue libraries = root.value(QLatin1String("libraries"));
    if (libraries.isArray()) {
        for (const QJsonValue& v : libraries.toArray()) {
            if (v.isObject()) { manifest.libraries.push_back(parseLicenseEntry(v.toObject())); }
        }
    }
    return manifest;
}

LicenseManifest loadBundledLicenseManifest() {
    QFile file(QStringLiteral(":/licenses/licenses.json"));
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    return parseLicenseManifest(file.readAll());
}

QString licenseDisplayName(const LicenseEntry& entry) {
    if (nonBlank(entry.name)) { return *entry.name; }
    QStringList parts;
    if (nonBlank(entry.group)) { parts << *entry.group; }
    if (nonBlank(entry.artifact)) { parts << *entry.artifact; }
    return parts.join(QLatin1Char(':'));
}

QString licenseVersionLabel(const LicenseEntry& entry) { return entry.version.value_or(QString()); }

std::optional<QString> licenseLabel(const LicenseEntry& entry) {
    if (entry.licenses.empty()) { return std::nullopt; }
    const std::optional<QString>& name = entry.licenses.front().name;
    if (nonBlank(name)) { return name; }
    return std::nullopt;
}

std::optional<QString> licenseClickUrl(const LicenseEntry& entry) {
    if (!entry.licenses.empty() && nonBlank(entry.licenses.front().url)) {
        return entry.licenses.front().url;
    }
    if (nonBlank(entry.url)) { return entry.url; }
    return std::nullopt;
}

bool licenseRowInteractive(const LicenseEntry& entry) { return licenseClickUrl(entry).has_value(); }

} // namespace dish::ui
