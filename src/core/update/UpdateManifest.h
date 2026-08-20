// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// latest.json (schema 1) parsing and validation — the ONLY reader of update
// metadata, so the reducer and checker downstream may trust a parsed manifest
// completely. Any violation of a required field is a typed ManifestError and
// the caller treats every error as a failed check.
//
// The release assets are deliberately not modelled: Dish never downloads its
// own update on Linux, so the manifest is read for the version and the notes
// link and nothing else.

#pragma once

#include "core/update/UpdateVersion.h"

#include <QByteArray>
#include <QString>

#include <variant>
#include <cstdint>

namespace dish::update {

// GitHub's Latest pointer; no api.github.com.
inline constexpr const char* kLatestManifestUrl =
    "https://github.com/TinkerNorth/dish-linux/releases/latest/download/latest.json";

// The body is size-checked BEFORE the JSON parse, because a captive portal's
// HTML splash can be arbitrarily large.
inline constexpr qint64 kManifestMaxBytes = qint64{64} * 1024;

enum class ManifestError : std::uint8_t {
    Oversize,          // body > 64 KiB before parsing
    BadJson,           // unparseable, or the root is not an object (portal HTML)
    UnsupportedSchema, // schema != 1 (greater = newer client required)
    WrongProduct,      // product != "dish-linux"
    WrongChannel,      // channel != "stable"
    BadVersion,        // version not strict M.m.p
    BadMinimum,        // minimumSupportedVersion malformed or > version
};

struct UpdateManifest {
    int schema = 1;
    QString product;
    QString version;
    QString channel;
    QString publishedAt; // display only, NEVER ordering
    QString minimumSupportedVersion;
    QString releaseNotesUrl; // "" when absent or dropped (non-https / non-github.com)

    bool operator==(const UpdateManifest& o) const {
        return schema == o.schema && product == o.product && version == o.version &&
               channel == o.channel && publishedAt == o.publishedAt &&
               minimumSupportedVersion == o.minimumSupportedVersion &&
               releaseNotesUrl == o.releaseNotesUrl;
    }
    bool operator!=(const UpdateManifest& o) const { return !(*this == o); }

    // Unknown extra fields are ignored (additive-only policy for schema 1).
    static std::variant<UpdateManifest, ManifestError> parse(const QByteArray& body);
};

} // namespace dish::update
