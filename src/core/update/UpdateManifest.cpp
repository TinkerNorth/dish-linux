// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/update/UpdateManifest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

namespace dish::update {

namespace {

// releaseNotesUrl is advisory: a bad value is DROPPED, not fatal, because the
// notes link must never be able to take the whole update check down.
QString sanitizedNotesUrl(const QJsonObject& root) {
    const QJsonValue value = root.value(QLatin1String("releaseNotesUrl"));
    if (!value.isString()) { return {}; }
    const QUrl url(value.toString(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
        url.host() != QLatin1String("github.com")) {
        return {};
    }
    return value.toString();
}

} // namespace

std::variant<UpdateManifest, ManifestError> UpdateManifest::parse(const QByteArray& body) {
    if (body.size() > kManifestMaxBytes) { return ManifestError::Oversize; }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return ManifestError::BadJson;
    }
    const QJsonObject root = doc.object();

    UpdateManifest m;

    const QJsonValue schema = root.value(QLatin1String("schema"));
    if (!schema.isDouble() || schema.toInt(-1) != 1) { return ManifestError::UnsupportedSchema; }
    m.schema = 1;

    m.product = root.value(QLatin1String("product")).toString();
    if (m.product != QLatin1String("dish-linux")) { return ManifestError::WrongProduct; }

    m.channel = root.value(QLatin1String("channel")).toString();
    if (m.channel != QLatin1String("stable")) { return ManifestError::WrongChannel; }

    m.version = root.value(QLatin1String("version")).toString();
    if (!isValidVersion(m.version)) { return ManifestError::BadVersion; }

    m.minimumSupportedVersion = root.value(QLatin1String("minimumSupportedVersion")).toString();
    if (!isValidVersion(m.minimumSupportedVersion) ||
        isStrictlyNewer(m.minimumSupportedVersion, m.version)) {
        return ManifestError::BadMinimum;
    }

    // Display only; carried verbatim, never ordered on.
    m.publishedAt = root.value(QLatin1String("publishedAt")).toString();

    m.releaseNotesUrl = sanitizedNotesUrl(root);

    return m;
}

} // namespace dish::update
