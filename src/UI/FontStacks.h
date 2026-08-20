// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The mono stack is probed by name rather than asking for the platform
// FixedFont, whose fontconfig alias resolves to whatever `monospace` happens to
// point at. `pickFamily` takes the available list as an argument, keeping the
// ordering rule testable without a font database.

#pragma once

#include <QFontDatabase>
#include <QGuiApplication>
#include <QString>
#include <QStringList>

namespace dish::ui {

// Case-insensitive: fontconfig's reported casing varies by family.
inline QString pickFamily(const QStringList& candidates, const QStringList& available,
                          const QString& fallback) {
    for (const QString& candidate : candidates) {
        for (const QString& family : available) {
            if (family.compare(candidate, Qt::CaseInsensitive) == 0) { return family; }
        }
    }
    return fallback;
}

inline QString preferredMonoFamily() {
    static const QStringList kCandidates{
        QStringLiteral("JetBrains Mono"),   QStringLiteral("Cascadia Mono"),
        QStringLiteral("Fira Mono"),        QStringLiteral("Source Code Pro"),
        QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono"),
        QStringLiteral("Noto Sans Mono")};
    return pickFamily(kCandidates, QFontDatabase::families(),
                      QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
}

// Inter is bundled; main.cpp registers it from :/fonts.
inline QString preferredSansFamily() {
    static const QStringList kCandidates{QStringLiteral("Inter"), QStringLiteral("Cantarell"),
                                         QStringLiteral("Noto Sans"),
                                         QStringLiteral("DejaVu Sans")};
    return pickFamily(kCandidates, QFontDatabase::families(), QGuiApplication::font().family());
}

} // namespace dish::ui
