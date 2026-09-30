// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What a satellite's catalog decides about a bound slot's touchpad: whether the
// type renders the DS4 pad, whether the host advertises mouse control, and so
// the touchpadMode the descriptor declares.

#include "core/reducer/CatalogFeatureGate.h"

#include "Models/Models.h"
#include "core/catalog/BundledCatalog.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QString>
#include <QStringList>

#include <initializer_list>
#include <optional>
#include <string>

namespace catalog = dish::catalog;
namespace proto = dish::proto;

using dish::models::CatalogDto;
using dish::models::CatalogFeatureDto;
using dish::models::CatalogHostFeatureDto;
using dish::models::CatalogTypeDto;
using dish::reducer::declaredTouchpadMode;
using dish::reducer::hostAdvertisesMouseControl;

namespace {

const QString kDs4Mode =
    QString::fromLatin1(proto::touchpadModeName(proto::kTouchpadModeDs4).data());
const std::optional<std::string> kPadPick{
    std::string(proto::touchpadModeName(proto::kTouchpadModeDs4))};

// A type as the satellite's catalog serves it: its touchpad either renders the
// "ds4" pad or is refused outright.
CatalogTypeDto catalogType(int id, bool rendersPad) {
    CatalogTypeDto type;
    type.id = id;
    CatalogFeatureDto touchpad;
    touchpad.supported = rendersPad;
    if (rendersPad) { touchpad.modes = QStringList{kDs4Mode}; }
    type.features.insert(catalog::kFeatureTouchpad, touchpad);
    return type;
}

CatalogDto catalogOf(std::initializer_list<CatalogTypeDto> types) {
    CatalogDto out;
    for (const auto& type : types) { out.controllerTypes.append(type); }
    return out;
}

CatalogHostFeatureDto advertisedHostFeature() {
    CatalogHostFeatureDto feature;
    feature.supported = true;
    return feature;
}

} // namespace

TEST_CASE("declared touchpad: a DualSense type whose catalog renders the pad declares it",
          "[catalog][touchpad-declared]") {
    // The satellite renders the DualSense touchpad as the "ds4" pad, the same
    // as the DualShock 4's.
    const auto catalog = catalogOf({catalogType(proto::kControllerTypeDualSense, true)});
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypeDualSense, catalog) ==
          proto::kTouchpadModeDs4);
}

TEST_CASE("declared touchpad: a DualShock 4 type its satellite renders no pad on declares off",
          "[catalog][touchpad-declared]") {
    const auto catalog = catalogOf({catalogType(proto::kControllerTypePlayStation, false)});
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypePlayStation, catalog) ==
          proto::kTouchpadModeOff);
}

TEST_CASE("declared touchpad: a type the satellite does not list declares off",
          "[catalog][touchpad-declared]") {
    const auto catalog = catalogOf({catalogType(proto::kControllerTypeXbox, false)});
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypePlayStation, catalog) ==
          proto::kTouchpadModeOff);
}

TEST_CASE("declared touchpad: before the catalog arrives the bundled table decides",
          "[catalog][touchpad-declared]") {
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypePlayStation, std::nullopt) ==
          proto::kTouchpadModeDs4);
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypeDualSense, std::nullopt) ==
          proto::kTouchpadModeDs4);
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypeSwitchPro, std::nullopt) ==
          proto::kTouchpadModeOff);
    CHECK(declaredTouchpadMode(kPadPick, true, proto::kControllerTypeXbox, std::nullopt) ==
          proto::kTouchpadModeOff);
}

TEST_CASE("declared touchpad: a host never picked for declares the pad render",
          "[catalog][touchpad-declared]") {
    const auto catalog = catalogOf({catalogType(proto::kControllerTypePlayStation, true)});
    CHECK(declaredTouchpadMode(std::nullopt, true, proto::kControllerTypePlayStation, catalog) ==
          proto::kTouchpadModeDs4);
}

TEST_CASE("declared touchpad: a Mouse pick declares off where the host advertises mouse control",
          "[catalog][touchpad-declared]") {
    // What a binding already stored as Mouse does: the ladder never falls back
    // to the pad render, and this client does not route the mouse, so it
    // declares off and requests no mouse control.
    auto served = catalogOf({catalogType(proto::kControllerTypePlayStation, true)});
    served.hostFeatures.insert(catalog::kHostFeatureMouseControl, advertisedHostFeature());
    const std::optional<std::string> mousePick{
        std::string(proto::touchpadModeName(proto::kTouchpadModeMouse))};
    CHECK(declaredTouchpadMode(mousePick, true, proto::kControllerTypePlayStation, served) ==
          proto::kTouchpadModeOff);
}

TEST_CASE("host mouse control: advertised only by a supported mouseControl feature",
          "[catalog][mouse]") {
    QHash<QString, CatalogHostFeatureDto> features;
    CHECK_FALSE(hostAdvertisesMouseControl(features));

    CatalogHostFeatureDto refused;
    refused.supported = false;
    features.insert(catalog::kHostFeatureMouseControl, refused);
    CHECK_FALSE(hostAdvertisesMouseControl(features));

    features.insert(catalog::kHostFeatureMouseControl, advertisedHostFeature());
    CHECK(hostAdvertisesMouseControl(features));
}

TEST_CASE("declared touchpad: a pad without a touchpad declares off",
          "[catalog][touchpad-declared]") {
    const auto catalog = catalogOf({catalogType(proto::kControllerTypePlayStation, true)});
    CHECK(declaredTouchpadMode(kPadPick, false, proto::kControllerTypePlayStation, catalog) ==
          proto::kTouchpadModeOff);
}
