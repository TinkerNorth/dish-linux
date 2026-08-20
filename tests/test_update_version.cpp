// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The updater's version grammar. It is deliberately a second implementation
// (the installer carries its own), so it gets its own pins: the release
// permalink never points at a prerelease, and a looser grammar is exactly how
// "0.10.0-rc1" ends up ordered below "0.9.0" and a fleet stops updating.
//
// isStrictlyNewer is the single predicate behind "stage only if strictly
// greater" and "apply only if staged > current", so its malformed-input
// behaviour (never newer) is the most load-bearing case here.

#include "core/update/UpdateVersion.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::update::compareVersions;
using dish::update::isStrictlyNewer;
using dish::update::isValidVersion;
using dish::update::UpdateVersion;

TEST_CASE("update version: the strict triple parses", "[update][version]") {
    REQUIRE(UpdateVersion::parse(QStringLiteral("0.0.0")).has_value());
    CHECK(*UpdateVersion::parse(QStringLiteral("0.1.0")) == UpdateVersion{0, 1, 0});
    CHECK(*UpdateVersion::parse(QStringLiteral("1.2.3")) == UpdateVersion{1, 2, 3});
    CHECK(*UpdateVersion::parse(QStringLiteral("10.20.30")) == UpdateVersion{10, 20, 30});
    CHECK(*UpdateVersion::parse(QStringLiteral("01.02.03")) == UpdateVersion{1, 2, 3});
    CHECK(*UpdateVersion::parse(QStringLiteral("2147483647.0.0")) ==
          UpdateVersion{2147483647, 0, 0});
    CHECK(isValidVersion(QStringLiteral("0.2.0")));
}

TEST_CASE("update version: anything the permalink never carries is rejected", "[update][version]") {
    for (const char* text : {"",
                             "1",
                             "1.2",
                             "1.2.3.4",
                             "1.2.",
                             ".2.3",
                             "1..3",
                             "v1.2.3",
                             "V1.2.3",
                             "1.2.3-rc1",
                             "1.2.3-alpha.1",
                             "1.2.3+build7",
                             " 1.2.3",
                             "1.2.3 ",
                             "1.2. 3",
                             "+1.2.3",
                             "-1.2.3",
                             "1.2.x",
                             "latest",
                             "0x1.0.0",
                             "2147483648.0.0",
                             "99999999999.0.0"}) {
        INFO("version: " << text);
        CHECK_FALSE(UpdateVersion::parse(QString::fromLatin1(text)).has_value());
        CHECK_FALSE(isValidVersion(QString::fromLatin1(text)));
    }
    // Non-ASCII digits are digits to QChar but not to this grammar.
    CHECK_FALSE(isValidVersion(QStringLiteral("１.２.３")));
}

TEST_CASE("update version: ordering is by triple, not by text", "[update][version]") {
    CHECK(UpdateVersion{0, 9, 0} < UpdateVersion{0, 10, 0}); // the lexicographic trap
    CHECK(UpdateVersion{1, 0, 0} > UpdateVersion{0, 99, 99});
    CHECK(UpdateVersion{1, 2, 3} == UpdateVersion{1, 2, 3});
    CHECK(UpdateVersion{1, 2, 3} != UpdateVersion{1, 3, 2});
    CHECK(UpdateVersion{1, 2, 3} <= UpdateVersion{1, 2, 3});
    CHECK(UpdateVersion{1, 2, 3} >= UpdateVersion{1, 2, 3});
    CHECK_FALSE(UpdateVersion{1, 2, 3} < UpdateVersion{1, 2, 3});
    CHECK_FALSE(UpdateVersion{1, 2, 3} > UpdateVersion{1, 2, 3});
    CHECK(UpdateVersion{2, 0, 0} > UpdateVersion{1, 99, 99});
    CHECK(UpdateVersion{1, 3, 0} > UpdateVersion{1, 2, 99});
    CHECK(UpdateVersion{1, 2, 4} > UpdateVersion{1, 2, 3});
}

TEST_CASE("update version: compareVersions is strcmp-shaped and total on valid input",
          "[update][version]") {
    CHECK(compareVersions(QStringLiteral("0.2.0"), QStringLiteral("0.2.0")) == 0);
    CHECK(*compareVersions(QStringLiteral("0.2.1"), QStringLiteral("0.2.0")) > 0);
    CHECK(*compareVersions(QStringLiteral("0.2.0"), QStringLiteral("0.2.1")) < 0);
    CHECK(*compareVersions(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")) > 0);
    CHECK(compareVersions(QStringLiteral("01.2.3"), QStringLiteral("1.2.3")) == 0);
    // A malformed side is nullopt: the caller decides what that means for it.
    CHECK_FALSE(compareVersions(QStringLiteral("0.2"), QStringLiteral("0.2.0")).has_value());
    CHECK_FALSE(compareVersions(QStringLiteral("0.2.0"), QStringLiteral("v0.2.0")).has_value());
    CHECK_FALSE(compareVersions(QString(), QString()).has_value());
}

TEST_CASE("update version: isStrictlyNewer is the stage-and-apply gate", "[update][version]") {
    CHECK(isStrictlyNewer(QStringLiteral("0.2.0"), QStringLiteral("0.1.0")));
    CHECK(isStrictlyNewer(QStringLiteral("0.10.0"), QStringLiteral("0.9.9")));
    CHECK(isStrictlyNewer(QStringLiteral("1.0.0"), QStringLiteral("0.99.99")));
    // Equal is NOT newer: this is what makes a post-apply loop impossible.
    CHECK_FALSE(isStrictlyNewer(QStringLiteral("0.2.0"), QStringLiteral("0.2.0")));
    CHECK_FALSE(isStrictlyNewer(QStringLiteral("0.1.0"), QStringLiteral("0.2.0")));
    // Malformed input can never rank above a real version, in either position.
    CHECK_FALSE(isStrictlyNewer(QStringLiteral("garbage"), QStringLiteral("0.1.0")));
    CHECK_FALSE(isStrictlyNewer(QStringLiteral("99.0.0-rc1"), QStringLiteral("0.1.0")));
    CHECK_FALSE(isStrictlyNewer(QStringLiteral("0.2.0"), QStringLiteral("garbage")));
    CHECK_FALSE(isStrictlyNewer(QString(), QString()));
}
