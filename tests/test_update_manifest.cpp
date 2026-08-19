// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// latest.json (schema 1). The golden document is the published example
// verbatim; every other case is one rule of the validation list, because this
// parser is the ONLY thing standing between a captive portal's HTML (or a
// lookalike host) and what the update pill tells the user to go install.

#include "core/update/UpdateManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QString>

#include <variant>

using dish::update::kLatestManifestUrl;
using dish::update::kManifestMaxBytes;
using dish::update::ManifestError;
using dish::update::UpdateManifest;

namespace {

QByteArray golden() {
    return "{\n"
           "  \"schema\": 1,\n"
           "  \"product\": \"dish-linux\",\n"
           "  \"version\": \"0.2.0\",\n"
           "  \"channel\": \"stable\",\n"
           "  \"publishedAt\": \"2026-08-03T14:21:07Z\",\n"
           "  \"minimumSupportedVersion\": \"0.1.0\",\n"
           "  \"releaseNotesUrl\": \"https://github.com/TinkerNorth/dish-linux/releases/tag/"
           "0.2.0\"\n"
           "}\n";
}

// The golden document with one field replaced, so each case differs from a
// KNOWN-GOOD manifest by exactly the rule it is testing.
QByteArray withField(const QByteArray& before, const QByteArray& after) {
    QByteArray json = golden();
    const qsizetype at = json.indexOf(before);
    REQUIRE(at >= 0);
    return json.replace(at, before.size(), after);
}

const UpdateManifest& ok(const std::variant<UpdateManifest, ManifestError>& r) {
    REQUIRE(std::holds_alternative<UpdateManifest>(r));
    return std::get<UpdateManifest>(r);
}

ManifestError err(const std::variant<UpdateManifest, ManifestError>& r) {
    REQUIRE(std::holds_alternative<ManifestError>(r));
    return std::get<ManifestError>(r);
}

} // namespace

TEST_CASE("the permalink is the release-latest pointer, not the API", "[update][manifest]") {
    const QString url = QString::fromLatin1(kLatestManifestUrl);
    CHECK(url.startsWith(QStringLiteral("https://github.com/TinkerNorth/dish-linux/releases/")));
    CHECK_FALSE(url.contains(QStringLiteral("api.github.com")));
}

TEST_CASE("the golden manifest parses to every field", "[update][manifest]") {
    const UpdateManifest m = ok(UpdateManifest::parse(golden()));
    CHECK(m.schema == 1);
    CHECK(m.product == QStringLiteral("dish-linux"));
    CHECK(m.version == QStringLiteral("0.2.0"));
    CHECK(m.channel == QStringLiteral("stable"));
    CHECK(m.publishedAt == QStringLiteral("2026-08-03T14:21:07Z"));
    CHECK(m.minimumSupportedVersion == QStringLiteral("0.1.0"));
    CHECK(m.releaseNotesUrl ==
          QStringLiteral("https://github.com/TinkerNorth/dish-linux/releases/tag/0.2.0"));
}

TEST_CASE("a body over the cap is rejected before the JSON parse", "[update][manifest]") {
    QByteArray big(kManifestMaxBytes + 1, 'x');
    CHECK(err(UpdateManifest::parse(big)) == ManifestError::Oversize);
}

TEST_CASE("a captive portal's HTML is BadJson, never a partial parse", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse("<html><body>Sign in to continue</body></html>")) ==
          ManifestError::BadJson);
    // A valid JSON scalar is still not a manifest.
    CHECK(err(UpdateManifest::parse("\"dish\"")) == ManifestError::BadJson);
}

TEST_CASE("a newer schema is refused rather than guessed at", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField("\"schema\": 1", "\"schema\": 2"))) ==
          ManifestError::UnsupportedSchema);
    CHECK(err(UpdateManifest::parse(withField("\"schema\": 1", "\"schema\": \"1\""))) ==
          ManifestError::UnsupportedSchema);
}

TEST_CASE("another product's manifest is refused", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(
              withField("\"product\": \"dish-linux\"", "\"product\": \"dish-windows\""))) ==
          ManifestError::WrongProduct);
}

TEST_CASE("a non-stable channel is refused", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField(
              "\"channel\": \"stable\"", "\"channel\": \"beta\""))) == ManifestError::WrongChannel);
}

TEST_CASE("version must be a strict M.m.p triple", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField("\"version\": \"0.2.0\"", "\"version\": \"0.2\""))) ==
          ManifestError::BadVersion);
    CHECK(err(UpdateManifest::parse(
              withField("\"version\": \"0.2.0\"", "\"version\": \"0.2.0-rc1\""))) ==
          ManifestError::BadVersion);
}

TEST_CASE("a minimum newer than the release itself is incoherent", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField("\"minimumSupportedVersion\": \"0.1.0\"",
                                              "\"minimumSupportedVersion\": \"0.3.0\""))) ==
          ManifestError::BadMinimum);
    CHECK(err(UpdateManifest::parse(withField("\"minimumSupportedVersion\": \"0.1.0\"",
                                              "\"minimumSupportedVersion\": \"nope\""))) ==
          ManifestError::BadMinimum);
}

TEST_CASE("releaseNotesUrl is advisory: a bad value is dropped, not fatal", "[update][manifest]") {
    SECTION("http is dropped") {
        const UpdateManifest m = ok(UpdateManifest::parse(withField(
            "\"releaseNotesUrl\": \"https://github.com/TinkerNorth/dish-linux/releases/tag/0.2.0\"",
            "\"releaseNotesUrl\": \"http://github.com/TinkerNorth/dish-linux/releases\"")));
        CHECK(m.releaseNotesUrl.isEmpty());
    }
    SECTION("a lookalike host is dropped") {
        const UpdateManifest m = ok(UpdateManifest::parse(withField(
            "\"releaseNotesUrl\": \"https://github.com/TinkerNorth/dish-linux/releases/tag/0.2.0\"",
            "\"releaseNotesUrl\": \"https://github.com.evil.example/TinkerNorth\"")));
        CHECK(m.releaseNotesUrl.isEmpty());
    }
    SECTION("a missing key is not an error") {
        QByteArray json = golden();
        const qsizetype at = json.indexOf(",\n  \"releaseNotesUrl\"");
        REQUIRE(at >= 0);
        json = json.left(at) + "\n}\n";
        const UpdateManifest m = ok(UpdateManifest::parse(json));
        CHECK(m.releaseNotesUrl.isEmpty());
        CHECK(m.version == QStringLiteral("0.2.0"));
    }
}

TEST_CASE("unknown fields are ignored, so the schema stays additive", "[update][manifest]") {
    QByteArray json = golden();
    const qsizetype at = json.indexOf("  \"schema\": 1,");
    REQUIRE(at >= 0);
    json.insert(at, "  \"somethingNewer\": { \"nested\": true },\n");
    const UpdateManifest m = ok(UpdateManifest::parse(json));
    CHECK(m.version == QStringLiteral("0.2.0"));
}
