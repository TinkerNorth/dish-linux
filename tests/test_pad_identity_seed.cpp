// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// seedControllerType has an identity-aware overload and an identity-less one
// that degrades to "first offered type". Calling the identity-less one for every
// bound slot is what declared a Switch Pro pad to the satellite as a virtual
// DualShock 4, so these pin the full ladder the call site must run: resolve the
// pad identity, format it as a vid:pid key, then seed with it.

#include "Models/Models.h"
#include "core/model/Protocol.h"
#include "core/reducer/BindingPresence.h"
#include "core/reducer/EmulateSeed.h"
#include "core/reducer/PickerVisibility.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace proto = dish::proto;
namespace reducer = dish::reducer;
using dish::models::CatalogDto;
using dish::models::CatalogEmulatesDto;
using dish::models::CatalogTypeDto;
using reducer::PresentSlot;

namespace {

constexpr int kSwitchProVid = 0x057e;
constexpr int kSwitchProPid = 0x2009;

PresentSlot present(const std::string& id, int vid, int pid) {
    PresentSlot s;
    s.id = id;
    s.vendorId = vid;
    s.productId = pid;
    return s;
}

CatalogTypeDto typeRow(int id, const QString& name, std::optional<CatalogEmulatesDto> emulates) {
    CatalogTypeDto t;
    t.id = id;
    t.name = name;
    t.shortName = name;
    t.emulates = std::move(emulates);
    return t;
}

CatalogEmulatesDto usbHint(const QString& vidPid) {
    CatalogEmulatesDto e;
    e.usb.push_back(vidPid);
    return e;
}

// Offers PlayStation first, the shape that turned every bound pad into a virtual
// DS4, but does carry `emulates` hints.
CatalogDto playStationFirstCatalog() {
    CatalogDto c;
    c.controllerTypes.push_back(typeRow(proto::kControllerTypePlayStation,
                                        QStringLiteral("DualShock 4"),
                                        usbHint(QStringLiteral("054c:05c4"))));
    c.controllerTypes.push_back(typeRow(proto::kControllerTypeSwitchPro,
                                        QStringLiteral("Switch Pro"),
                                        usbHint(QStringLiteral("057e:2009"))));
    c.controllerTypes.push_back(
        typeRow(proto::kControllerTypeXbox, QStringLiteral("Xbox 360"), std::nullopt));
    return c;
}

// Exactly what the call site must do: slot id -> pad identity -> seed.
int seedForSlot(const std::string& slotId, const std::vector<PresentSlot>& shown,
                const std::optional<CatalogDto>& catalog,
                std::optional<int> userOverride = std::nullopt) {
    const auto identity = reducer::padIdentityFor(slotId, shown);
    const QString vidPid =
        identity ? reducer::vidPidKey(identity->first, identity->second) : QString();
    return reducer::seedControllerType(userOverride, catalog, /*sdlTypeSlug=*/QString(), vidPid);
}

} // namespace

TEST_CASE("padIdentityFor: resolves the pad behind a shown slot", "[padidentity]") {
    const std::vector<PresentSlot> shown = {present("sdl:0", kSwitchProVid, kSwitchProPid)};
    const auto identity = reducer::padIdentityFor("sdl:0", shown);
    REQUIRE(identity.has_value());
    REQUIRE(identity->first == kSwitchProVid);
    REQUIRE(identity->second == kSwitchProPid);
}

TEST_CASE("padIdentityFor: a slot that is not shown resolves to nothing", "[padidentity]") {
    REQUIRE_FALSE(reducer::padIdentityFor("sdl:0", {}).has_value());
}

TEST_CASE("padIdentityFor: a 0:0 descriptor is identity-less, not a key", "[padidentity]") {
    const std::vector<PresentSlot> shown = {present("sdl:0", 0, 0)};
    REQUIRE_FALSE(reducer::padIdentityFor("sdl:0", shown).has_value());
}

TEST_CASE("seed: a Switch Pro is NOT declared a DualShock 4 by a PlayStation-first catalog",
          "[padidentity][catalog]") {
    const std::vector<PresentSlot> shown = {present("sdl:0", kSwitchProVid, kSwitchProPid)};
    const int seeded = seedForSlot("sdl:0", shown, playStationFirstCatalog());
    REQUIRE(seeded != proto::kControllerTypePlayStation);
    REQUIRE(seeded == proto::kControllerTypeSwitchPro);
}

TEST_CASE("seed: the identity is resolved through a USB-direct synthetic slot too",
          "[padidentity][catalog]") {
    // (0x057e << 16) | 0x2009 — the synthetic slot id a claimed Switch Pro gets.
    const std::vector<PresentSlot> shown = {present("92151817", kSwitchProVid, kSwitchProPid)};
    REQUIRE(seedForSlot("92151817", shown, playStationFirstCatalog()) ==
            proto::kControllerTypeSwitchPro);
}

TEST_CASE("seed: a pad with no matching hint still takes the catalog's first offered type",
          "[padidentity][catalog]") {
    // First-offered is still the documented fallback; only dropping the identity
    // before consulting it was wrong.
    const std::vector<PresentSlot> shown = {present("sdl:0", 0x20d6, 0xa713)};
    REQUIRE(seedForSlot("sdl:0", shown, playStationFirstCatalog()) ==
            proto::kControllerTypePlayStation);
}

TEST_CASE("seed: the user's Emulate override still wins over the pad identity",
          "[padidentity][catalog]") {
    const std::vector<PresentSlot> shown = {present("sdl:0", kSwitchProVid, kSwitchProPid)};
    REQUIRE(seedForSlot("sdl:0", shown, playStationFirstCatalog(),
                        proto::kControllerTypePlayStation) == proto::kControllerTypePlayStation);
}

TEST_CASE("seed: an unresolvable slot degrades to first-offered, never to a stranger's hint",
          "[padidentity][catalog]") {
    REQUIRE(seedForSlot("sdl:0", {}, playStationFirstCatalog()) ==
            proto::kControllerTypePlayStation);
}
