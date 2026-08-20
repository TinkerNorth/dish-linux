// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Third-party-attribution DTO, parse, and row-mapping rules. Mirrors
// dish-android's LicenseManifest.kt / LicensesAdapter.kt. The parse tolerates
// unknown keys and missing fields: forward-compat is part of the contract.

#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace dish::ui {

struct LicenseInfo {
    std::optional<QString> name;
    std::optional<QString> url;
};

struct LicenseEntry {
    std::optional<QString> group;
    std::optional<QString> artifact;
    std::optional<QString> version;
    std::optional<QString> name;
    std::optional<QString> url;
    std::vector<LicenseInfo> licenses;
};

struct LicenseManifest {
    std::optional<QString> generatedBy;
    std::vector<LicenseEntry> libraries;
};

// A malformed / empty / non-object document yields an empty manifest, no throw.
LicenseManifest parseLicenseManifest(const QByteArray& json);

LicenseManifest loadBundledLicenseManifest();

// entry.name if non-blank, else group:artifact.
QString licenseDisplayName(const LicenseEntry& entry);

QString licenseVersionLabel(const LicenseEntry& entry);

// nullopt makes the row hide the label rather than render an empty one.
std::optional<QString> licenseLabel(const LicenseEntry& entry);

// licenses[0].url, else entry.url; nullopt leaves the row non-interactive.
std::optional<QString> licenseClickUrl(const LicenseEntry& entry);

bool licenseRowInteractive(const LicenseEntry& entry);

} // namespace dish::ui
