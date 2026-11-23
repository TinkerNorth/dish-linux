// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The decisions of the controller database check (docs/controller-db.md
// section 6.3 and 6.4), pure so the coordinator only executes them: whether a
// request may leave the machine at all, what the manifest's answer means, which
// paths a manifest may name, whether a downloaded file is the one it promised,
// and the result word the last check is remembered by.

#pragma once

#include "core/controllerdb/ControllerRecord.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::controllerdb {

inline constexpr std::string_view kDbBaseUrl = "https://dish.tinkernorth.com/controllers/v1/";
inline constexpr std::string_view kManifestPath = "manifest.json";
inline constexpr std::string_view kRecordsPath = "controllers.json";

// Ten seconds after launch, then every six hours while running.
inline constexpr std::int64_t kFirstCheckDelayMs = 10'000;
inline constexpr std::int64_t kCheckIntervalMs = std::int64_t{6} * 60 * 60 * 1000;

// Refused before parsing: a captive portal answers with whatever it likes.
inline constexpr std::int64_t kManifestMaxBytes = std::int64_t{64} * 1024;
inline constexpr std::int64_t kFileMaxBytes = std::int64_t{8} * 1024 * 1024;

enum class CheckResult : std::uint8_t { UpToDate, Updated, Offline, Invalid, Off };

inline std::string_view checkResultToken(CheckResult result) {
    switch (result) {
    case CheckResult::UpToDate:
        return "upToDate";
    case CheckResult::Updated:
        return "updated";
    case CheckResult::Offline:
        return "offline";
    case CheckResult::Invalid:
        return "invalid";
    case CheckResult::Off:
        return "off";
    }
    return "invalid";
}

inline std::optional<CheckResult> checkResultFrom(std::string_view token) {
    constexpr std::array<CheckResult, 5> kAll{CheckResult::UpToDate, CheckResult::Updated,
                                              CheckResult::Offline, CheckResult::Invalid,
                                              CheckResult::Off};
    for (const CheckResult result : kAll) {
        if (checkResultToken(result) == token) { return result; }
    }
    return std::nullopt;
}

enum class CheckTrigger : std::uint8_t { Schedule, CheckNow };

// The switch is the whole rule: off means no request leaves the machine, "Check now" included.
inline bool checkMayRun(bool checksEnabled, CheckTrigger /*trigger*/) { return checksEnabled; }

// The ETag is only worth sending for the database it was given with; otherwise a 304 would keep
// a copy the app no longer holds.
inline std::optional<std::string> ifNoneMatchFor(const std::string& savedEtag, int savedForVersion,
                                                 int activeVersion) {
    const bool sameDatabase = !savedEtag.empty() && savedForVersion == activeVersion;
    return sameDatabase ? std::optional<std::string>(savedEtag) : std::nullopt;
}

// What a reply's transport and status come to before its body is looked at. Only 200 brings
// bytes to verify and 304 says nothing is new; a reply that never arrived and every other status
// (a 404 before the site deploys, a 408, a 429, a server error) is offline, because nothing came
// that could fail verification. "invalid" is for bytes that arrived and did not verify.
enum class ReplyVerdict : std::uint8_t { Body, NotModified, Offline };

inline ReplyVerdict replyVerdict(bool transportFailed, int httpStatus) {
    constexpr int kOk = 200;
    constexpr int kNotModified = 304;
    if (transportFailed) { return ReplyVerdict::Offline; }
    if (httpStatus == kNotModified) { return ReplyVerdict::NotModified; }
    if (httpStatus == kOk) { return ReplyVerdict::Body; }
    return ReplyVerdict::Offline;
}

// The files this build fetches and writes, and so the only paths ever fetched or written.
inline constexpr std::array<std::string_view, 7> kDbFilePaths{
    "controllers.json", "strings/en.json", "strings/bs.json",   "strings/de.json",
    "strings/es.json",  "strings/fr.json", "strings/pt-BR.json"};

inline bool isDbFilePath(std::string_view path) {
    return std::find(kDbFilePaths.begin(), kDbFilePaths.end(), path) != kDbFilePaths.end();
}

namespace detail {

inline bool isLocaleCharacter(char ch) {
    const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = ch >= '0' && ch <= '9';
    return letter || digit || ch == '-';
}

// "strings/<locale>.json" with a locale of letters, digits and hyphens that starts with a letter:
// a strings file a later database may add for a locale this build does not ship. It is never
// fetched, so nothing a manifest names here becomes a path on disk.
inline bool isLaterStringsPath(std::string_view path) {
    constexpr std::string_view kPrefix = "strings/";
    constexpr std::string_view kSuffix = ".json";
    constexpr std::size_t kMaxLocaleLength = 35;
    const bool shaped = path.size() > kPrefix.size() + kSuffix.size() &&
                        path.substr(0, kPrefix.size()) == kPrefix &&
                        path.substr(path.size() - kSuffix.size()) == kSuffix;
    if (!shaped) { return false; }
    const std::string_view locale =
        path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
    const bool startsWithALetter = (locale.front() >= 'a' && locale.front() <= 'z') ||
                                   (locale.front() >= 'A' && locale.front() <= 'Z');
    const bool localeCharacters = std::all_of(locale.begin(), locale.end(), &isLocaleCharacter);
    return startsWithALetter && localeCharacters && locale.size() <= kMaxLocaleLength;
}

inline bool isLowerHex64(std::string_view text) {
    constexpr std::size_t kSha256HexLength = 64;
    if (text.size() != kSha256HexLength) { return false; }
    return std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

inline bool fileIsWellFormed(const ManifestFile& file) {
    const bool knownPath = isDbFilePath(file.path) || isLaterStringsPath(file.path);
    const bool hexDigest = isLowerHex64(file.sha256);
    const bool plausibleSize = file.bytes > 0 && file.bytes <= kFileMaxBytes;
    return knownPath && hexDigest && plausibleSize;
}

inline bool names(const DbManifest& manifest, std::string_view path) {
    return std::any_of(manifest.files.begin(), manifest.files.end(),
                       [path](const ManifestFile& file) { return file.path == path; });
}

} // namespace detail

// A manifest worth acting on: schema 1, a positive dbVersion, the records and the English
// strings among its files, and every file a database path (one this build ships, or a strings
// file for a locale it does not) with a digest and a sane size.
inline bool manifestIsUsable(const DbManifest& manifest) {
    const bool schemaOne = manifest.schema == kSchema;
    const bool versioned = manifest.dbVersion > 0;
    const bool hasRecords = detail::names(manifest, kRecordsPath);
    const bool hasEnglish = detail::names(manifest, "strings/en.json");
    const bool everyFileWellFormed =
        std::all_of(manifest.files.begin(), manifest.files.end(), &detail::fileIsWellFormed);
    return schemaOne && versioned && hasRecords && hasEnglish && everyFileWellFormed;
}

// The manifest's files this build fetches, verifies and stores, in the manifest's order: a
// strings file for a locale it does not ship is left out, never a reason to refuse the set.
inline std::vector<ManifestFile> filesThisBuildFetches(const DbManifest& manifest) {
    std::vector<ManifestFile> out;
    std::copy_if(manifest.files.begin(), manifest.files.end(), std::back_inserter(out),
                 [](const ManifestFile& file) { return isDbFilePath(file.path); });
    return out;
}

// After a usable manifest: nothing to do unless it publishes a later database than the one in
// force. A dbVersion only grows, so an equal or lower one is "up to date".
enum class ManifestStep : std::uint8_t { UpToDate, Download };

inline ManifestStep manifestStep(const DbManifest& manifest, int activeVersion) {
    return manifest.dbVersion > activeVersion ? ManifestStep::Download : ManifestStep::UpToDate;
}

// The exact bytes the manifest promised: its size and its SHA-256, lowercase hex.
inline bool fileMatches(const ManifestFile& promised, std::string_view sha256Hex,
                        std::int64_t bytes) {
    return promised.bytes == bytes && promised.sha256 == sha256Hex;
}

// The downloaded records, parsed: schema 1 and the database the manifest announced.
inline bool recordsMatchManifest(const ControllerDb& db, const DbManifest& manifest) {
    return db.schema == kSchema && db.dbVersion == manifest.dbVersion;
}

} // namespace dish::controllerdb
