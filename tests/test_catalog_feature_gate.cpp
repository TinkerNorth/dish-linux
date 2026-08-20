// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// allowedCapsForType is UNWIRED in both clients today — dish-linux and
// dish-windows both record it as landed-but-not-called in their PARITY ledgers.
//
// That is exactly why the rule is pinned here: the intersection has no call
// site to break, so nothing but these cases stands between it and a quiet
// semantic change before whoever wires it up gets there.
//
// The rule: a bit survives only when the pad detected it AND the type's catalog
// offers the matching slug. The four-slug table is the whole of this function's
// business; the rest of the caps word is somebody else's.

#include "core/reducer/CatalogFeatureGate.h"

#include "Models/Models.h"
#include "core/catalog/BundledCatalog.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace catalog = dish::catalog;
namespace proto = dish::proto;

using dish::models::CatalogFeatureDto;
using dish::models::CatalogTypeDto;
using dish::reducer::allowedCapsForType;

namespace {

CatalogTypeDto typeOffering(std::initializer_list<std::pair<QString, bool>> features) {
    CatalogTypeDto type;
    for (const auto& [slug, supported] : features) {
        CatalogFeatureDto feature;
        feature.supported = supported;
        type.features.insert(slug, feature);
    }
    return type;
}

// switchpro as the catalog publishes it: real motion, analogTriggers declared
// unsupported — the case the header's own comment names.
CatalogTypeDto switchPro() {
    CatalogTypeDto type = typeOffering({{catalog::kFeatureAnalogTriggers, false},
                                        {catalog::kFeatureRumble, true},
                                        {catalog::kFeatureMotion, true}});
    type.slug = catalog::kSlugSwitchPro;
    return type;
}

CatalogTypeDto dualSense() {
    CatalogTypeDto type = typeOffering({{catalog::kFeatureAnalogTriggers, true},
                                        {catalog::kFeatureRumble, true},
                                        {catalog::kFeatureMotion, true},
                                        {catalog::kFeatureLightbar, true}});
    type.slug = catalog::kSlugDualSense;
    return type;
}

constexpr std::uint16_t kAllFour = static_cast<std::uint16_t>(
    proto::kCapAnalogTriggers | proto::kCapRumble | proto::kCapMotion | proto::kCapLightbar);

// Bits a newer protocol invents, or that never had a slug. 0x0010 is the next
// one a touchpad cap would claim, which is why it is the probe here.
constexpr std::uint16_t kUnmappedBits = 0x8010;

} // namespace

TEST_CASE("allowedCapsForType: switchpro strips the analog triggers the pad really has",
          "[catalog][feature-gate]") {
    const std::uint16_t allowed = allowedCapsForType(kAllFour, switchPro());
    CHECK((allowed & proto::kCapAnalogTriggers) == 0);
    // Lightbar is absent from the type's features entirely: also not offered.
    CHECK((allowed & proto::kCapLightbar) == 0);
    CHECK(allowed == static_cast<std::uint16_t>(proto::kCapRumble | proto::kCapMotion));
}

TEST_CASE("allowedCapsForType: a bit whose slug the type offers survives",
          "[catalog][feature-gate]") {
    CHECK(allowedCapsForType(kAllFour, dualSense()) == kAllFour);
    // One bit at a time, so a wholesale pass-through cannot fake this.
    CHECK(allowedCapsForType(proto::kCapMotion, switchPro()) == proto::kCapMotion);
    CHECK(allowedCapsForType(proto::kCapRumble, switchPro()) == proto::kCapRumble);
}

TEST_CASE("allowedCapsForType: a bit the pad never detected is not invented",
          "[catalog][feature-gate]") {
    // The intersection is one-way: the catalog may only take bits away.
    CHECK(allowedCapsForType(proto::kCapRumble, dualSense()) == proto::kCapRumble);
    CHECK(allowedCapsForType(0, dualSense()) == 0);
    CHECK(allowedCapsForType(0, switchPro()) == 0);
}

TEST_CASE("allowedCapsForType: bits outside the four-slug table pass through untouched",
          "[catalog][feature-gate]") {
    const CatalogTypeDto barren = typeOffering({});
    const auto detected = static_cast<std::uint16_t>(kAllFour | kUnmappedBits);
    // Every mapped bit goes, every unmapped bit stays — including touchpad's,
    // which has no caps bit at all and rides the descriptor's touchpadMode.
    CHECK(allowedCapsForType(detected, barren) == kUnmappedBits);
    CHECK(allowedCapsForType(kUnmappedBits, switchPro()) == kUnmappedBits);
}

TEST_CASE("allowedCapsForType: the touchpad slug maps to no bit", "[catalog][feature-gate]") {
    const CatalogTypeDto touchOnly = typeOffering({{catalog::kFeatureTouchpad, true}});
    CHECK(allowedCapsForType(kAllFour, touchOnly) == 0);
}

TEST_CASE("allowedCapsForType: an unknown feature slug strips nothing", "[catalog][feature-gate]") {
    CatalogTypeDto type = dualSense();
    CHECK(allowedCapsForType(kAllFour, type) == kAllFour);

    // A slug from a newer server that this client has no code for: inert here,
    // whichever way its `supported` reads.
    CatalogFeatureDto warp;
    warp.supported = false;
    type.features.insert(QStringLiteral("warp"), warp);
    CHECK(allowedCapsForType(kAllFour, type) == kAllFour);

    warp.supported = true;
    type.features.insert(QStringLiteral("warp"), warp);
    CHECK(allowedCapsForType(kAllFour, type) == kAllFour);
}

TEST_CASE("allowedCapsForType: an unknown slug cannot vouch for a mapped bit",
          "[catalog][feature-gate]") {
    // The other side of the same rule: only the four known slugs can KEEP a
    // bit, so a type that describes its rumble under an invented name loses it.
    const CatalogTypeDto renamed = typeOffering({{QStringLiteral("hdRumble"), true}});
    CHECK(allowedCapsForType(proto::kCapRumble, renamed) == 0);
}
