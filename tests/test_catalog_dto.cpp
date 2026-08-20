// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Forward compatibility only: a controller type newer than the app must still
// parse and render from server strings. The base parse lives in test_models.cpp.

#include "Models/Models.h"
#include "composer/CatalogComposer.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>

using dish::composer::knownTypeSlugs;
using dish::composer::offerableTypes;
using dish::composer::PickableType;
using dish::models::CatalogDto;

namespace {
QJsonObject parse(const char* json) { return QJsonDocument::fromJson(QByteArray(json)).object(); }
} // namespace

TEST_CASE("CatalogDto tolerates an unknown controller slug + unknown feature keys",
          "[catalog][forwardcompat]") {
    const auto catalog = CatalogDto::fromJson(parse(R"({
        "locale":"de",
        "protocolVersion":1,
        "serverVersion":"1.6.0",
        "controllerTypes":[
          {"id":0,"slug":"xbox360","name":"Xbox 360 Controller","shortName":"Xbox",
           "description":"d","image":{"href":"/api/catalog/images/xbox360","etag":"\"1.6.0\""},
           "features":{"rumble":{"supported":true},"motion":{"supported":false}}},
          {"id":7,"slug":"hyperpad","name":"HyperPad 9000","shortName":"Hyper",
           "description":"future","image":{"href":"/api/catalog/images/hyperpad","etag":"x"},
           "features":{"warp":{"supported":true,"requires":"fluxcap>=2"}}}
        ],
        "hostFeatures":{"mouseControl":{"supported":true,"modes":["off","ds4","mouse"]}}
    })"));

    REQUIRE(catalog.locale == "de");
    REQUIRE(catalog.controllerTypes.size() == 2);
    REQUIRE(catalog.controllerTypes[1].id == 7);
    REQUIRE(catalog.controllerTypes[1].name == "HyperPad 9000");
    REQUIRE(catalog.controllerTypes[1].features.contains("warp"));
    REQUIRE(catalog.controllerTypes[1].features.value("warp").supported);
    REQUIRE(catalog.controllerTypes[1].features.value("warp").requires_.has_value());
    REQUIRE(*catalog.controllerTypes[1].features.value("warp").requires_ == "fluxcap>=2");
    REQUIRE(catalog.hostFeatures.value("mouseControl").modes ==
            QStringList({"off", "ds4", "mouse"}));
}

TEST_CASE("offerableTypes renders both known and unknown types from the catalog",
          "[catalog][forwardcompat]") {
    const auto catalog = CatalogDto::fromJson(parse(R"({
        "locale":"en","protocolVersion":1,"serverVersion":"1.6.0",
        "controllerTypes":[
          {"id":0,"slug":"xbox360","name":"Xbox 360 Controller","shortName":"Xbox","description":"a"},
          {"id":1,"slug":"ds4","name":"DualShock 4","shortName":"PlayStation","description":"b"},
          {"id":2,"slug":"dualsense","name":"DualSense","shortName":"DualSense","description":"c"},
          {"id":3,"slug":"switchpro","name":"Switch Pro","shortName":"Switch","description":"d"},
          {"id":7,"slug":"hyperpad","name":"HyperPad 9000","shortName":"Hyper","description":"e"}
        ],
        "hostFeatures":{}
    })"));

    const QList<PickableType> rows = offerableTypes(catalog);
    REQUIRE(rows.size() == 5);
    // Catalog order is preserved.
    REQUIRE(rows[0].type == 0);
    REQUIRE(rows[0].slug == "xbox360");
    REQUIRE(rows[0].known); // `known` = the client bundles art/translations
    REQUIRE(rows[1].slug == "ds4");
    REQUIRE(rows[1].known);
    REQUIRE(rows[2].type == 2);
    REQUIRE(rows[2].slug == "dualsense");
    REQUIRE(rows[2].known);
    REQUIRE(rows[3].type == 3);
    REQUIRE(rows[3].slug == "switchpro");
    REQUIRE(rows[3].known);
    REQUIRE(rows[4].type == 7);
    REQUIRE(rows[4].name == "HyperPad 9000");
    REQUIRE_FALSE(rows[4].known);

    const QList<QString> known = knownTypeSlugs();
    REQUIRE(known.contains(QStringLiteral("dualsense")));
    REQUIRE(known.contains(QStringLiteral("switchpro")));
}

TEST_CASE("offerableTypes drops a nameless catalog row", "[catalog][forwardcompat]") {
    const auto catalog = CatalogDto::fromJson(parse(R"({
        "locale":"en","protocolVersion":1,"serverVersion":"1.6.0",
        "controllerTypes":[
          {"id":0,"slug":"xbox360","name":"Xbox 360 Controller","shortName":"Xbox"},
          {"id":9,"slug":"ghost"}
        ],
        "hostFeatures":{}
    })"));

    const QList<PickableType> rows = offerableTypes(catalog);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].slug == "xbox360");
}

TEST_CASE("offerableTypes on an empty/never-loaded catalog yields no rows",
          "[catalog][forwardcompat]") {
    CatalogDto empty;
    REQUIRE(offerableTypes(empty).isEmpty());
}
