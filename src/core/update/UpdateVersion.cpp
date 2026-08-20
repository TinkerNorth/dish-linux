// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/update/UpdateVersion.h"

namespace dish::update {

namespace {

// One dotted component: nonempty, digits only, fits an int. QString::toInt is
// not enough on its own — it accepts "+1" and surrounding whitespace.
std::optional<int> parseComponent(QStringView part) {
    if (part.isEmpty()) { return std::nullopt; }
    for (const QChar ch : part) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) { return std::nullopt; }
    }
    bool ok = false;
    const int value = part.toInt(&ok, 10);
    if (!ok || value < 0) { return std::nullopt; }
    return value;
}

} // namespace

std::optional<UpdateVersion> UpdateVersion::parse(const QString& text) {
    const QStringView view(text);
    const qsizetype firstDot = view.indexOf(QLatin1Char('.'));
    if (firstDot < 0) { return std::nullopt; }
    const qsizetype secondDot = view.indexOf(QLatin1Char('.'), firstDot + 1);
    if (secondDot < 0) { return std::nullopt; }
    // A third dot means a fourth component; strict grammar has exactly three.
    if (view.indexOf(QLatin1Char('.'), secondDot + 1) >= 0) { return std::nullopt; }

    const auto major = parseComponent(view.left(firstDot));
    const auto minor = parseComponent(view.mid(firstDot + 1, secondDot - firstDot - 1));
    const auto patch = parseComponent(view.mid(secondDot + 1));
    if (!major || !minor || !patch) { return std::nullopt; }
    return UpdateVersion{*major, *minor, *patch};
}

bool isValidVersion(const QString& text) { return UpdateVersion::parse(text).has_value(); }

std::optional<int> compareVersions(const QString& a, const QString& b) {
    const auto va = UpdateVersion::parse(a);
    const auto vb = UpdateVersion::parse(b);
    if (!va || !vb) { return std::nullopt; }
    if (*va < *vb) { return -1; }
    if (*vb < *va) { return 1; }
    return 0;
}

bool isStrictlyNewer(const QString& candidate, const QString& baseline) {
    const auto cmp = compareVersions(candidate, baseline);
    return cmp.has_value() && *cmp > 0;
}

} // namespace dish::update
