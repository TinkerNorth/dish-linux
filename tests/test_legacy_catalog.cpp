// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The catalog-version normalization, which is the one place the app is allowed
// to know that catalogs used to look different.
//
// It matters in a way its size hides. A legacy satellite's body is NOT trusted:
// only its version is read, and this client substitutes its own representation
// of what that version means. Get the substitution wrong and a user on an older
// satellite sees an empty controller picker, or a DualShock 4 whose touchpad the
// resolver refuses, and neither failure points back here.
//
// Run at the repository boundary on both a 200-fill and a stale-serve, so it is
// applied to the same body more than once and must be idempotent.

#include "core/catalog/LegacyCatalogTranslator.h"

#include "composer/CatalogComposer.h"
#include "core/catalog/BundledCatalog.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::catalog::kCatalogVersionCurrent;
using dish::catalog::kCatalogVersionLegacyV1;
using dish::catalog::kFeatureTouchpad;
using dish::catalog::kSlugDs4;
using dish::catalog::kSlugXbox360;
using dish::catalog::normalizeCatalog;
namespace m = dish::models;

namespace {

// A body carrying everything the translator must NOT touch, plus one
// controller type it must throw away.
m::CatalogDto fetchedAt(int catalogVersion) {
    m::CatalogDto c;
    c.catalogVersion = catalogVersion;
    c.locale = QStringLiteral("de-DE");
    c.protocolVersion = 3;
    c.serverVersion = QStringLiteral("2.1.0");
    c.etag = QStringLiteral("2.1.0+de-DE");
    c.httpStatus = 200;
    c.notModified = false;
    c.reachable = true;

    m::CatalogTypeDto bogus;
    bogus.id = 99;
    bogus.slug = QStringLiteral("not-a-real-pad");
    bogus.name = QStringLiteral("Something The Host Made Up");
    c.controllerTypes = {bogus};

    m::CatalogHostFeatureDto rumble;
    rumble.supported = true;
    c.hostFeatures.insert(QStringLiteral("rumble"), rumble);
    return c;
}

const m::CatalogTypeDto* typeWithSlug(const m::CatalogDto& c, const QString& slug) {
    for (const auto& t : c.controllerTypes) {
        if (t.slug == slug) { return &t; }
    }
    return nullptr;
}

} // namespace

TEST_CASE("legacy catalog: a current-or-newer catalog passes through untouched",
          "[catalog][legacy]") {
    // Deliberately >= rather than a per-version case: a newer-than-current
    // catalog is additive within protocolVersion 1, so refusing it here would
    // break a client against a satellite that is ahead of it.
    for (int v :
         {kCatalogVersionCurrent, kCatalogVersionCurrent + 1, kCatalogVersionCurrent + 50}) {
        INFO("catalogVersion " << v);
        const auto fetched = fetchedAt(v);
        const auto out = normalizeCatalog(fetched);
        CHECK(out.catalogVersion == v);
        REQUIRE(out.controllerTypes.size() == 1);
        // Including the type this client has never heard of: at or above current
        // the body IS the truth.
        CHECK(out.controllerTypes.at(0).slug == QStringLiteral("not-a-real-pad"));
    }
}

TEST_CASE("legacy catalog: a legacy body's controller types are replaced, not merged",
          "[catalog][legacy]") {
    const auto out = normalizeCatalog(fetchedAt(kCatalogVersionLegacyV1));
    CHECK(out.catalogVersion == kCatalogVersionLegacyV1);
    REQUIRE(out.controllerTypes.size() == 2);
    CHECK(typeWithSlug(out, QStringLiteral("not-a-real-pad")) == nullptr);
    CHECK(typeWithSlug(out, kSlugXbox360) != nullptr);
    CHECK(typeWithSlug(out, kSlugDs4) != nullptr);
}

TEST_CASE("legacy catalog: everything that is not a controller type passes through",
          "[catalog][legacy]") {
    const auto fetched = fetchedAt(kCatalogVersionLegacyV1);
    const auto out = normalizeCatalog(fetched);
    CHECK(out.locale == fetched.locale);
    CHECK(out.protocolVersion == fetched.protocolVersion);
    CHECK(out.serverVersion == fetched.serverVersion);
    CHECK(out.etag == fetched.etag);
    CHECK(out.httpStatus == fetched.httpStatus);
    CHECK(out.notModified == fetched.notModified);
    CHECK(out.reachable == fetched.reachable);
    // Host features are the HOST's answer about itself, which a schema version
    // says nothing about.
    CHECK(out.hostFeatures.size() == 1);
    CHECK(out.hostFeatures.value(QStringLiteral("rumble")).supported);
}

TEST_CASE("legacy catalog: a version below v1 is still read as v1", "[catalog][legacy]") {
    // An absent catalogVersion parses as 1, but a malformed one can arrive as 0
    // or negative, and there is no older representation to fall back to.
    for (int v : {0, -1, kCatalogVersionLegacyV1}) {
        INFO("catalogVersion " << v);
        const auto out = normalizeCatalog(fetchedAt(v));
        CHECK(out.catalogVersion == kCatalogVersionLegacyV1);
        CHECK(out.controllerTypes.size() == 2);
    }
}

TEST_CASE("legacy catalog: the substituted types are offerable", "[catalog][legacy]") {
    // The picker drops a nameless row, so a substitution that carried only slugs
    // would blank the controller picker for every legacy satellite. This is the
    // failure the header's comment is about, asserted through the projection
    // that actually does the dropping.
    const auto out = normalizeCatalog(fetchedAt(kCatalogVersionLegacyV1));
    const auto offerable = dish::composer::offerableTypes(out);
    REQUIRE(offerable.size() == 2);
    for (const auto& row : offerable) { CHECK_FALSE(row.name.isEmpty()); }
}

TEST_CASE("legacy catalog: the substituted ds4 advertises the ds4 touchpad mode",
          "[catalog][legacy]") {
    // The resolver gates the touchpad on the mode slug, not on the feature
    // alone, so a substitution that declared the feature and no modes would give
    // a DualShock 4 whose touchpad silently never works.
    const auto out = normalizeCatalog(fetchedAt(kCatalogVersionLegacyV1));
    const auto* ds4 = typeWithSlug(out, kSlugDs4);
    REQUIRE(ds4 != nullptr);
    const auto touchpad = ds4->features.value(kFeatureTouchpad);
    CHECK(touchpad.supported);
    const auto expected = dish::proto::touchpadModeName(dish::proto::kTouchpadModeDs4);
    REQUIRE(touchpad.modes.size() == 1);
    CHECK(touchpad.modes.at(0) ==
          QString::fromUtf8(expected.data(), static_cast<qsizetype>(expected.size())));

    // And the Xbox pad has no touchpad at all, or the picker would offer one.
    const auto* xbox = typeWithSlug(out, kSlugXbox360);
    REQUIRE(xbox != nullptr);
    CHECK_FALSE(xbox->features.contains(kFeatureTouchpad));
}

TEST_CASE("legacy catalog: the substituted types carry the protocol's own ids",
          "[catalog][legacy]") {
    // The id is what goes on the wire in a controller descriptor. A substitution
    // with the wrong one asks the satellite to build the other pad.
    const auto out = normalizeCatalog(fetchedAt(kCatalogVersionLegacyV1));
    CHECK(typeWithSlug(out, kSlugXbox360)->id == dish::proto::kControllerTypeXbox);
    CHECK(typeWithSlug(out, kSlugDs4)->id == dish::proto::kControllerTypePlayStation);
}

TEST_CASE("legacy catalog: normalizing twice is normalizing once", "[catalog][legacy]") {
    // It runs on a 200-fill AND on a stale-serve of the cache it filled, so the
    // second pass sees its own output.
    const auto once = normalizeCatalog(fetchedAt(kCatalogVersionLegacyV1));
    const auto twice = normalizeCatalog(once);
    REQUIRE(twice.controllerTypes.size() == once.controllerTypes.size());
    for (int i = 0; i < once.controllerTypes.size(); ++i) {
        const auto& a = once.controllerTypes.at(i);
        const auto& b = twice.controllerTypes.at(i);
        INFO("type " << a.slug.toStdString());
        CHECK(b.id == a.id);
        CHECK(b.slug == a.slug);
        CHECK(b.name == a.name);
        CHECK(b.shortName == a.shortName);
        CHECK(b.features.size() == a.features.size());
    }
    CHECK(twice.catalogVersion == once.catalogVersion);
    CHECK(twice.etag == once.etag);
}
