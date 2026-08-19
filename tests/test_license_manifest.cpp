// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The shipped-manifest case reads assets/licenses/licenses.json through
// DISH_LICENSES_JSON_PATH: the Qt resource lives only in the Dish exe, not in
// dish_core, which is what this test links.

#include "UI/licenses/LicenseManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>

using dish::ui::licenseClickUrl;
using dish::ui::licenseDisplayName;
using dish::ui::licenseLabel;
using dish::ui::LicenseManifest;
using dish::ui::licenseRowInteractive;
using dish::ui::licenseVersionLabel;
using dish::ui::parseLicenseManifest;

namespace {
LicenseManifest parse(const char* json) { return parseLicenseManifest(QByteArray(json)); }
} // namespace

TEST_CASE("LicenseManifest parses a well-formed manifest", "[licenses][manifest]") {
    const auto m = parse(R"({
        "generatedBy":"x",
        "libraries":[
          {"group":"io.qt","artifact":"qtbase","version":"6.7.3","name":"Qt 6",
           "url":"https://qt.io","licenses":[{"name":"LGPL-3.0","url":"https://l/lgpl"}]}
        ]
    })");
    REQUIRE(m.generatedBy.has_value());
    REQUIRE(m.libraries.size() == 1);
    const auto& e = m.libraries.front();
    REQUIRE(licenseDisplayName(e) == QStringLiteral("Qt 6"));
    REQUIRE(licenseVersionLabel(e) == QStringLiteral("6.7.3"));
    REQUIRE(e.licenses.size() == 1);
}

TEST_CASE("LicenseManifest is forward-compatible with unknown keys", "[licenses][manifest]") {
    const auto m = parse(R"({
        "generatedBy":"gen",
        "futureField":{"nested":true},
        "libraries":[
          {"group":"g","artifact":"a","version":"1.0","name":"Lib","unknownEntryKey":42,
           "licenses":[{"name":"MIT","url":"u","extra":"ignored"}]}
        ]
    })");
    REQUIRE(m.libraries.size() == 1);
    const auto& e = m.libraries.front();
    REQUIRE(licenseDisplayName(e) == QStringLiteral("Lib"));
    REQUIRE(licenseVersionLabel(e) == QStringLiteral("1.0"));
    REQUIRE(licenseLabel(e).has_value());
    REQUIRE(*licenseLabel(e) == QStringLiteral("MIT"));
}

TEST_CASE("licenseDisplayName falls back to group:artifact when name is blank",
          "[licenses][manifest]") {
    const auto m1 = parse(R"({"libraries":[{"group":"io.qt","artifact":"qtbase"}]})");
    REQUIRE(licenseDisplayName(m1.libraries.front()) == QStringLiteral("io.qt:qtbase"));

    // A present-but-empty name behaves as absent.
    const auto m2 = parse(R"({"libraries":[{"group":"g","artifact":"a","name":""}]})");
    REQUIRE(licenseDisplayName(m2.libraries.front()) == QStringLiteral("g:a"));

    // Only one of group/artifact present -> no dangling colon.
    const auto m3 = parse(R"({"libraries":[{"artifact":"solo"}]})");
    REQUIRE(licenseDisplayName(m3.libraries.front()) == QStringLiteral("solo"));

    const auto m4 = parse(R"({"libraries":[{"group":"g","artifact":"a","name":"Pretty"}]})");
    REQUIRE(licenseDisplayName(m4.libraries.front()) == QStringLiteral("Pretty"));
}

TEST_CASE("licenseLabel is hidden when licenses empty or first name blank",
          "[licenses][manifest]") {
    const auto m1 = parse(R"({"libraries":[{"name":"L"}]})");
    REQUIRE_FALSE(licenseLabel(m1.libraries.front()).has_value());

    const auto m2 = parse(R"({"libraries":[{"name":"L","licenses":[]}]})");
    REQUIRE_FALSE(licenseLabel(m2.libraries.front()).has_value());

    const auto m3 = parse(R"({"libraries":[{"name":"L","licenses":[{"name":"","url":"u"}]}]})");
    REQUIRE_FALSE(licenseLabel(m3.libraries.front()).has_value());

    const auto m4 = parse(R"({"libraries":[{"name":"L","licenses":[{"name":"BSD"}]}]})");
    REQUIRE(licenseLabel(m4.libraries.front()).has_value());
    REQUIRE(*licenseLabel(m4.libraries.front()) == QStringLiteral("BSD"));
}

TEST_CASE("licenseClickUrl: licenses[0].url wins over entry.url; absent -> inert",
          "[licenses][manifest]") {
    const auto m1 = parse(
        R"({"libraries":[{"url":"https://entry","licenses":[{"name":"L","url":"https://lic"}]}]})");
    REQUIRE(licenseClickUrl(m1.libraries.front()).has_value());
    REQUIRE(*licenseClickUrl(m1.libraries.front()) == QStringLiteral("https://lic"));
    REQUIRE(licenseRowInteractive(m1.libraries.front()));

    const auto m2 = parse(R"({"libraries":[{"url":"https://entry","licenses":[{"name":"L"}]}]})");
    REQUIRE(*licenseClickUrl(m2.libraries.front()) == QStringLiteral("https://entry"));

    const auto m3 = parse(R"({"libraries":[{"name":"L","licenses":[{"name":"L"}]}]})");
    REQUIRE_FALSE(licenseClickUrl(m3.libraries.front()).has_value());
    REQUIRE_FALSE(licenseRowInteractive(m3.libraries.front()));
}

TEST_CASE("LicenseManifest: malformed / empty JSON yields an empty manifest (no crash)",
          "[licenses][manifest]") {
    REQUIRE(parse("").libraries.empty());
    REQUIRE(parse("not json at all {{{").libraries.empty());
    REQUIRE(parse("[1,2,3]").libraries.empty());
    REQUIRE(parse("{}").libraries.empty());
    REQUIRE(parse(R"({"libraries":"oops"})").libraries.empty());
}

TEST_CASE("shipped manifest: non-empty, every row has a name, Qt6 present",
          "[licenses][manifest]") {
    QFile file(QStringLiteral(DISH_LICENSES_JSON_PATH));
    REQUIRE(file.open(QIODevice::ReadOnly));
    const LicenseManifest m = parseLicenseManifest(file.readAll());

    // Qt6 is the LGPL compliance artifact; it has to be in the shipped list.
    REQUIRE_FALSE(m.libraries.empty());
    bool sawQt = false;
    for (const auto& e : m.libraries) {
        REQUIRE_FALSE(licenseDisplayName(e).isEmpty());
        if (licenseDisplayName(e).contains(QStringLiteral("Qt"))) { sawQt = true; }
    }
    REQUIRE(sawQt);
}
