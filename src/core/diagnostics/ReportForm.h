// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// "Report a problem" and "Ask a question" (docs/diagnostics-bundle.md, "The
// report"): the category the person picked, their own words, and the
// controller it concerns, which go first in the mail and into the file as
// `report`. The words for a category are the person's language, so the page
// passes them in; the tokens and the shapes of the subject and body are here.

#pragma once

#include "core/diagnostics/MailHandoff.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dish::diagnostics {

enum class ReportCategory : std::uint8_t {
    NotRecognised,
    NoFeedback,
    Connection,
    Audio,
    Other,
    Question,
};

struct ReportCategoryRow {
    ReportCategory category = ReportCategory::Other;
    std::string_view token;
};

// The page's order, which is the contract's.
inline constexpr std::array<ReportCategoryRow, 6> kReportCategories{{
    {ReportCategory::NotRecognised, "notRecognised"},
    {ReportCategory::NoFeedback, "noFeedback"},
    {ReportCategory::Connection, "connection"},
    {ReportCategory::Audio, "audio"},
    {ReportCategory::Other, "other"},
    {ReportCategory::Question, "question"},
}};

inline std::string_view reportCategoryToken(ReportCategory category) {
    for (const ReportCategoryRow& row : kReportCategories) {
        if (row.category == category) { return row.token; }
    }
    return "other";
}

inline std::optional<ReportCategory> reportCategoryFromToken(std::string_view token) {
    for (const ReportCategoryRow& row : kReportCategories) {
        if (row.token == token) { return row.category; }
    }
    return std::nullopt;
}

// "Dish support: No rumble or feedback · Dish for Linux 2.8.0 on AYN Thor".
inline std::string reportSubject(std::string_view categoryWords, std::string_view version,
                                 std::string_view model) {
    std::string subject = "Dish support: ";
    subject += categoryWords;
    subject += " \xC2\xB7 ";
    subject += kAppDisplayName;
    subject += " ";
    subject += version;
    subject += " on ";
    subject += model;
    return subject;
}

namespace detail {

inline std::string_view withoutSurroundingSpace(std::string_view text) {
    const auto isSpace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && isSpace(text[first])) { ++first; }
    while (last > first && isSpace(text[last - 1])) { --last; }
    return text.substr(first, last - first);
}

} // namespace detail

// The person's words, a blank line, then the page's summary; the summary alone when they wrote
// nothing.
inline std::string reportMailBody(std::string_view text, std::string_view summary) {
    const std::string_view words = detail::withoutSurroundingSpace(text);
    if (words.empty()) { return std::string(summary); }
    std::string body(words);
    body += "\n\n";
    body += summary;
    return body;
}

// The record id the pad matched, else the product string it reports, else nothing.
inline std::optional<std::string> reportControllerOf(const std::optional<std::string>& recordId,
                                                     std::string_view product) {
    if (recordId.has_value()) { return recordId; }
    if (product.empty()) { return std::nullopt; }
    return std::string(product);
}

} // namespace dish::diagnostics
