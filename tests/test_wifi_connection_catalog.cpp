// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Thin-catalog: the sent controller type + touchpad mode come from the
// satellite catalog, defaulting to its first offered type (physical-pad
// matching deferred). desiredDescriptor() is the seam — no socket/session
// needed, so the wiring is drivable directly.

#include "Models/Models.h"
#include "Models/Protocol.h"
#include "Network/WifiConnection.h"

#include <catch2/catch_test_macros.hpp>

using dish::models::CatalogType;
using dish::models::DiscoveredServer;
using dish::models::ServerCatalog;
using dish::net::WifiConnection;

TEST_CASE("descriptor type defaults to the first catalog entry", "[catalog]") {
    SECTION("ds4-first catalog sends type 1 with the ds4 touchpad mode") {
        WifiConnection conn(QStringLiteral("ds4-first"), DiscoveredServer{});
        conn.attachSlot(QStringLiteral("slot-1"), /*hasLightbar=*/false, /*hasMotion=*/false);
        ServerCatalog cat;
        cat.controllerTypes = {CatalogType{1, true}, CatalogType{0, false}};
        conn.setCatalog(cat);
        REQUIRE(conn.catalogFetched());
        const auto desc = conn.desiredDescriptor();
        REQUIRE(desc.has_value());
        CHECK(desc->type == 1);
        CHECK(desc->touchpadMode == dish::proto::kTouchpadModeDs4);
    }

    SECTION("xbox-first catalog sends type 0 with touchpad off") {
        WifiConnection conn(QStringLiteral("xbox-first"), DiscoveredServer{});
        conn.attachSlot(QStringLiteral("slot-1"), /*hasLightbar=*/false, /*hasMotion=*/false);
        ServerCatalog cat;
        cat.controllerTypes = {CatalogType{0, false}, CatalogType{1, true}};
        conn.setCatalog(cat);
        const auto desc = conn.desiredDescriptor();
        REQUIRE(desc.has_value());
        CHECK(desc->type == 0);
        CHECK(desc->touchpadMode == dish::proto::kTouchpadModeOff);
    }
}

TEST_CASE("no catalog falls back to type 0 / touchpad off", "[catalog]") {
    WifiConnection conn(QStringLiteral("no-catalog"), DiscoveredServer{});
    conn.attachSlot(QStringLiteral("slot-1"), /*hasLightbar=*/false, /*hasMotion=*/false);
    REQUIRE_FALSE(conn.catalogFetched());
    const auto desc = conn.desiredDescriptor();
    REQUIRE(desc.has_value());
    CHECK(desc->type == dish::proto::kControllerTypeXbox);
    CHECK(desc->touchpadMode == dish::proto::kTouchpadModeOff);
}
