// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Diagnostics by email (docs/controller-db.md section 8,
// docs/diagnostics-bundle.md "The mail"): the file is saved under the app's
// data folder, then handed to the person's own mail client through
// `xdg-email --attach <file> --subject <s> --body <b> support@tinkernorth.com`;
// when xdg-email is missing or does not take it, a mailto: link opens with the
// subject and the body. Either way the folder is revealed, so the person can
// attach the file when the mail app leaves it out. Nothing leaves the machine
// until they press Send. The decisions are here; starting the processes is the
// system layer's.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dish::diagnostics {

inline constexpr std::string_view kSupportAddress = "support@tinkernorth.com";
inline constexpr std::string_view kAppName = "dish-linux";
inline constexpr std::string_view kAppDisplayName = "Dish for Linux";

struct CivilTime {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;

    bool operator==(const CivilTime&) const = default;
};

// UTC from milliseconds since the epoch, by days-from-civil's inverse (Howard Hinnant's
// algorithm), so the bundle's times need no time-zone database.
inline CivilTime civilUtc(std::int64_t epochMs) {
    constexpr std::int64_t kMsPerSecond = 1000;
    constexpr std::int64_t kSecondsPerDay = 86400;
    std::int64_t seconds = epochMs / kMsPerSecond;
    if (epochMs % kMsPerSecond < 0) { --seconds; }
    std::int64_t days = seconds / kSecondsPerDay;
    std::int64_t secondOfDay = seconds % kSecondsPerDay;
    if (secondOfDay < 0) {
        secondOfDay += kSecondsPerDay;
        --days;
    }
    const std::int64_t shifted = days + 719468;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const std::int64_t dayOfEra = shifted - era * 146097;
    const std::int64_t yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t monthIndex = (5 * dayOfYear + 2) / 153;
    const std::int64_t day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const std::int64_t month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    return CivilTime{static_cast<int>(year),
                     static_cast<int>(month),
                     static_cast<int>(day),
                     static_cast<int>(secondOfDay / 3600),
                     static_cast<int>((secondOfDay % 3600) / 60),
                     static_cast<int>(secondOfDay % 60)};
}

namespace detail {

inline std::string padded(int value, int width) {
    std::string digits = std::to_string(value);
    while (static_cast<int>(digits.size()) < width) { digits.insert(digits.begin(), '0'); }
    return digits;
}

} // namespace detail

// "2026-11-19T21:04:11Z"
inline std::string isoUtc(std::int64_t epochMs) {
    const CivilTime t = civilUtc(epochMs);
    return detail::padded(t.year, 4) + "-" + detail::padded(t.month, 2) + "-" +
           detail::padded(t.day, 2) + "T" + detail::padded(t.hour, 2) + ":" +
           detail::padded(t.minute, 2) + ":" + detail::padded(t.second, 2) + "Z";
}

// "dish-diagnostics-dish-linux-20261119-210411.json", the person's local time.
inline std::string bundleFileName(const CivilTime& local) {
    return "dish-diagnostics-" + std::string(kAppName) + "-" + detail::padded(local.year, 4) +
           detail::padded(local.month, 2) + detail::padded(local.day, 2) + "-" +
           detail::padded(local.hour, 2) + detail::padded(local.minute, 2) +
           detail::padded(local.second, 2) + ".json";
}

inline constexpr std::string_view kXdgEmail = "xdg-email";

// RevealFolder follows a hand-off that went: xdg-utils 1.1.3 attaches the file for Thunderbird
// only and hands every other mail app a mailto: link without it, exiting 0 all the same.
enum class MailStep : std::uint8_t { RunXdgEmail, RevealFolder, OpenMailtoAndRevealFolder };

// The first step: xdg-email where it is on the PATH (xdg-utils is no hard dependency of any
// package, and a minimal desktop or a sandbox may not have it), the mailto: fallback where it is
// not.
inline MailStep firstMailStep(bool xdgEmailFound) {
    return xdgEmailFound ? MailStep::RunXdgEmail : MailStep::OpenMailtoAndRevealFolder;
}

// How the xdg-email process ended: it never started, it exited with a code, or it is still
// running when the grace period ends, which a mail client launched in the foreground does.
enum class XdgEmailEnd : std::uint8_t { FailedToStart, Exited, StillRunning };

// xdg-email's exit codes (xdg-utils): 0 the mail client took the message; 1 a syntax error, 2 a
// file that does not exist, 3 a tool it needs is missing, 4 the action failed, 5 no permission
// to read the file. Only 0 hands the mail over; everything else falls back. A process still
// running has a compose window open, so it is handed over too. 0 does not mean the file is
// attached (see RevealFolder).
inline constexpr int kXdgEmailSuccess = 0;

inline MailStep nextStepAfterXdgEmail(XdgEmailEnd end, int exitCode) {
    switch (end) {
    case XdgEmailEnd::FailedToStart:
        return MailStep::OpenMailtoAndRevealFolder;
    case XdgEmailEnd::Exited:
        return exitCode == kXdgEmailSuccess ? MailStep::RevealFolder
                                            : MailStep::OpenMailtoAndRevealFolder;
    case XdgEmailEnd::StillRunning:
        return MailStep::RevealFolder;
    }
    return MailStep::OpenMailtoAndRevealFolder;
}

// xdg-email's arguments, in the order the contract names them; each is one argv entry, so
// nothing in the subject or body is ever read by a shell.
inline std::vector<std::string> xdgEmailArguments(std::string_view attachmentPath,
                                                  std::string_view subject, std::string_view body,
                                                  std::string_view to) {
    return {"--attach", std::string(attachmentPath), "--subject",    std::string(subject),
            "--body",   std::string(body),           std::string(to)};
}

namespace detail {

inline bool isUnreserved(unsigned char ch) {
    const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = ch >= '0' && ch <= '9';
    return alpha || digit || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

inline void appendEncoded(std::string& out, unsigned char ch) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    if (isUnreserved(ch)) {
        out.push_back(static_cast<char>(ch));
        return;
    }
    out.push_back('%');
    out.push_back(kHex[static_cast<std::size_t>(ch >> 4)]);
    out.push_back(kHex[static_cast<std::size_t>(ch & 0x0F)]);
}

// The byte length of the UTF-8 sequence a lead byte starts; a stray continuation byte is one.
inline std::size_t utf8SequenceLength(unsigned char lead) {
    if (lead >= 0xF0) { return 4; }
    if (lead >= 0xE0) { return 3; }
    if (lead >= 0xC0) { return 2; }
    return 1;
}

inline std::string encoded(std::string_view text) {
    std::string out;
    for (const char ch : text) { appendEncoded(out, static_cast<unsigned char>(ch)); }
    return out;
}

} // namespace detail

// A mailto: link a shell can open: past `maxLength` the body is cut at a whole character,
// because mail clients and the shell refuse longer links and a cut one still has its subject.
inline constexpr std::size_t kMailtoMaxLength = 2000;

inline std::string mailtoUrl(std::string_view to, std::string_view subject, std::string_view body,
                             std::size_t maxLength = kMailtoMaxLength) {
    std::string url = "mailto:" + std::string(to) + "?subject=" + detail::encoded(subject);
    const std::string bodyKey = "&body=";
    if (url.size() + bodyKey.size() >= maxLength) { return url; }
    url += bodyKey;
    std::size_t at = 0;
    while (at < body.size()) {
        const auto lead = static_cast<unsigned char>(body[at]);
        const std::size_t length = std::min(detail::utf8SequenceLength(lead), body.size() - at);
        const std::string piece = detail::encoded(body.substr(at, length));
        if (url.size() + piece.size() > maxLength) { break; }
        url += piece;
        at += length;
    }
    return url;
}

} // namespace dish::diagnostics
