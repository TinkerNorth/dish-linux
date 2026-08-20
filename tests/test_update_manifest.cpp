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
    CHECK(url ==
          QStringLiteral("https://github.com/TinkerNorth/dish-linux/releases/latest/download/"
                         "latest.json"));
    CHECK_FALSE(url.contains(QStringLiteral("api.github.com")));
    CHECK(kManifestMaxBytes == qint64{64} * 1024);
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
    QByteArray big(static_cast<qsizetype>(kManifestMaxBytes) + 1, 'x');
    CHECK(err(UpdateManifest::parse(big)) == ManifestError::Oversize);
}

TEST_CASE("a body of exactly the cap still parses", "[update][manifest]") {
    // The cap is inclusive, so `>` and `>=` are not interchangeable here.
    QByteArray atCap = golden();
    atCap.append(QByteArray(static_cast<qsizetype>(kManifestMaxBytes) - atCap.size(), ' '));
    REQUIRE(atCap.size() == kManifestMaxBytes);
    CHECK(ok(UpdateManifest::parse(atCap)).version == QStringLiteral("0.2.0"));
}

TEST_CASE("a captive portal's HTML is BadJson, never a partial parse", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse("<html><body>Sign in to continue</body></html>")) ==
          ManifestError::BadJson);
    // A valid JSON scalar is still not a manifest.
    CHECK(err(UpdateManifest::parse("\"dish\"")) == ManifestError::BadJson);
    CHECK(err(UpdateManifest::parse("[]")) == ManifestError::BadJson);
    CHECK(err(UpdateManifest::parse("null")) == ManifestError::BadJson);
    // A truncated body is the shape a cut-off transfer produces.
    CHECK(err(UpdateManifest::parse("{\"schema\": 1,")) == ManifestError::BadJson);
    CHECK(err(UpdateManifest::parse(QByteArray())) == ManifestError::BadJson);
}

TEST_CASE("a newer schema is refused rather than guessed at", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField("\"schema\": 1", "\"schema\": 2"))) ==
          ManifestError::UnsupportedSchema);
    CHECK(err(UpdateManifest::parse(withField("\"schema\": 1", "\"schema\": 0"))) ==
          ManifestError::UnsupportedSchema);
    CHECK(err(UpdateManifest::parse(withField("\"schema\": 1", "\"schema\": \"1\""))) ==
          ManifestError::UnsupportedSchema);
    // No schema key at all must not default-accept as schema 1.
    CHECK(err(UpdateManifest::parse(withField("\"schema\": 1", "\"notSchema\": 1"))) ==
          ManifestError::UnsupportedSchema);
}

TEST_CASE("another product's manifest is refused", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(
              withField("\"product\": \"dish-linux\"", "\"product\": \"dish-windows\""))) ==
          ManifestError::WrongProduct);
    // An empty product is a missing one; neither may pass as this client's.
    CHECK(err(UpdateManifest::parse(withField(
              "\"product\": \"dish-linux\"", "\"product\": \"\""))) == ManifestError::WrongProduct);
}

TEST_CASE("a non-stable channel is refused", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField(
              "\"channel\": \"stable\"", "\"channel\": \"beta\""))) == ManifestError::WrongChannel);
    // The compare is exact, so a wrong-case channel is a different channel.
    CHECK(err(UpdateManifest::parse(
              withField("\"channel\": \"stable\"", "\"channel\": \"Stable\""))) ==
          ManifestError::WrongChannel);
}

TEST_CASE("version must be a strict M.m.p triple", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField("\"version\": \"0.2.0\"", "\"version\": \"0.2\""))) ==
          ManifestError::BadVersion);
    CHECK(err(UpdateManifest::parse(
              withField("\"version\": \"0.2.0\"", "\"version\": \"0.2.0-rc1\""))) ==
          ManifestError::BadVersion);
    // The tag carries a "v", the version field never does.
    CHECK(err(UpdateManifest::parse(withField(
              "\"version\": \"0.2.0\"", "\"version\": \"v0.2.0\""))) == ManifestError::BadVersion);
    // A JSON number reads back as an empty string, which must not pass.
    CHECK(err(UpdateManifest::parse(withField("\"version\": \"0.2.0\"", "\"version\": 2"))) ==
          ManifestError::BadVersion);
}

TEST_CASE("a minimum newer than the release itself is incoherent", "[update][manifest]") {
    CHECK(err(UpdateManifest::parse(withField("\"minimumSupportedVersion\": \"0.1.0\"",
                                              "\"minimumSupportedVersion\": \"0.3.0\""))) ==
          ManifestError::BadMinimum);
    CHECK(err(UpdateManifest::parse(withField("\"minimumSupportedVersion\": \"0.1.0\"",
                                              "\"minimumSupportedVersion\": \"nope\""))) ==
          ManifestError::BadMinimum);
    // Equal is legitimate — everything older is unsupported — so the bound is
    // `>` and not `>=`.
    CHECK(ok(UpdateManifest::parse(withField("\"minimumSupportedVersion\": \"0.1.0\"",
                                             "\"minimumSupportedVersion\": \"0.2.0\"")))
              .minimumSupportedVersion == QStringLiteral("0.2.0"));
}

TEST_CASE("releaseNotesUrl is advisory: a bad value is dropped, not fatal", "[update][manifest]") {
    // Every case swaps only the notes link, so the manifest around it stays
    // known-good and a dropped link is never confused with a rejected manifest.
    const auto notes = [](const QByteArray& url) {
        return ok(UpdateManifest::parse(withField(
                      "\"releaseNotesUrl\": \"https://github.com/TinkerNorth/dish-linux/releases/"
                      "tag/0.2.0\"",
                      "\"releaseNotesUrl\": \"" + url + "\"")))
            .releaseNotesUrl;
    };
    SECTION("another github link on the same host is kept verbatim") {
        CHECK(notes("https://github.com/TinkerNorth/dish-linux/releases") ==
              QStringLiteral("https://github.com/TinkerNorth/dish-linux/releases"));
    }
    SECTION("http is dropped") {
        CHECK(notes("http://github.com/TinkerNorth/dish-linux/releases").isEmpty());
    }
    SECTION("a lookalike host is dropped") {
        CHECK(notes("https://github.com.evil.example/TinkerNorth").isEmpty());
        CHECK(notes("https://evil.example/notes").isEmpty());
    }
    SECTION("a scheme that is not https is dropped, so the UI can never be handed one") {
        CHECK(notes("javascript:alert(1)").isEmpty());
    }
    SECTION("an unparsable value is dropped rather than failing the check") {
        CHECK(notes("not a url").isEmpty());
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
