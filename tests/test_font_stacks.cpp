// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Asking Qt for the platform FixedFont generic resolves to whatever
// fontconfig's `monospace` alias points at, and mono carries every Hz, IP and
// latency readout in the UI. pickFamily takes the available list as an argument
// so the rule is testable against a synthetic database, identically on every
// machine.

#include "UI/FontStacks.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

using dish::ui::pickFamily;

TEST_CASE("pickFamily returns the first CANDIDATE present, not the first available",
          "[fonts][stacks]") {
    const QStringList candidates{QStringLiteral("JetBrains Mono"), QStringLiteral("Fira Mono")};
    const QStringList available{QStringLiteral("Fira Mono"), QStringLiteral("JetBrains Mono"),
                                QStringLiteral("DejaVu Sans")};
    REQUIRE(pickFamily(candidates, available, QStringLiteral("monospace")) ==
            QStringLiteral("JetBrains Mono"));
}

TEST_CASE("pickFamily walks down the stack when the preferred family is missing",
          "[fonts][stacks]") {
    const QStringList candidates{QStringLiteral("JetBrains Mono"), QStringLiteral("Fira Mono"),
                                 QStringLiteral("DejaVu Sans Mono")};
    REQUIRE(pickFamily(candidates,
                       {QStringLiteral("Fira Mono"), QStringLiteral("DejaVu Sans Mono")},
                       QStringLiteral("monospace")) == QStringLiteral("Fira Mono"));
    REQUIRE(pickFamily(candidates, {QStringLiteral("DejaVu Sans Mono")},
                       QStringLiteral("monospace")) == QStringLiteral("DejaVu Sans Mono"));
}

TEST_CASE("pickFamily falls back only when no candidate is installed", "[fonts][stacks]") {
    const QStringList candidates{QStringLiteral("JetBrains Mono"), QStringLiteral("Fira Mono")};
    REQUIRE(pickFamily(candidates, {QStringLiteral("DejaVu Sans"), QStringLiteral("monospace")},
                       QStringLiteral("monospace")) == QStringLiteral("monospace"));
    REQUIRE(pickFamily(candidates, {}, QStringLiteral("monospace")) == QStringLiteral("monospace"));
    REQUIRE(pickFamily({}, {QStringLiteral("Fira Mono")}, QStringLiteral("monospace")) ==
            QStringLiteral("monospace"));
}

TEST_CASE("pickFamily matches case-insensitively and keeps the database spelling",
          "[fonts][stacks]") {
    // fontconfig's reported casing varies by family, and the returned name must
    // be the one the database knows.
    REQUIRE(pickFamily({QStringLiteral("Fira Mono")}, {QStringLiteral("fira mono")},
                       QStringLiteral("monospace")) == QStringLiteral("fira mono"));
}

TEST_CASE("the mono stack never yields the generic alias while a real mono is installed",
          "[fonts][stacks]") {
    const QStringList candidates{QStringLiteral("JetBrains Mono"), QStringLiteral("Fira Mono"),
                                 QStringLiteral("DejaVu Sans Mono")};
    const QStringList available{QStringLiteral("monospace"), QStringLiteral("Fira Mono"),
                                QStringLiteral("DejaVu Serif")};
    const QString picked = pickFamily(candidates, available, QStringLiteral("monospace"));
    REQUIRE(picked != QStringLiteral("monospace"));
    REQUIRE(picked == QStringLiteral("Fira Mono"));
}

TEST_CASE("the sans stack prefers the bundled Inter over the system face", "[fonts][stacks]") {
    // Inter ships in the qrc and is registered at startup, so it is present on
    // every machine; the later rungs cover a build without it.
    const QStringList candidates{QStringLiteral("Inter"), QStringLiteral("Cantarell"),
                                 QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")};
    REQUIRE(pickFamily(candidates,
                       {QStringLiteral("Cantarell"), QStringLiteral("Inter"),
                        QStringLiteral("DejaVu Sans")},
                       QStringLiteral("sans-serif")) == QStringLiteral("Inter"));
    REQUIRE(pickFamily(candidates, {QStringLiteral("DejaVu Sans"), QStringLiteral("Cantarell")},
                       QStringLiteral("sans-serif")) == QStringLiteral("Cantarell"));
}
