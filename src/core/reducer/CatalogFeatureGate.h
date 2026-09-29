// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Per-type capability gating against the satellite's catalog. A client must not
// claim a proto::kCap* bit the type's catalog features do not offer: switchpro
// advertises analogTriggers supported:false, so a pad with real triggers still
// must not claim the cap on that type.

#pragma once

#include "Models/Models.h"
#include "core/catalog/BundledCatalog.h"
#include "core/model/Protocol.h"
#include "core/reducer/PickerVisibility.h"
#include "core/reducer/TouchpadModeResolve.h"

#include <QString>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dish::reducer {

// A bit survives only when the pad detected it and the type offers the matching
// slug. Bits outside the protocol-1 vocabulary pass through untouched: this owns
// the per-slug intersection, not the word's full shape. Touchpad is absent from
// the table because it has no caps bit; it rides the descriptor's touchpadMode.
inline std::uint16_t allowedCapsForType(std::uint16_t detectedCaps,
                                        const models::CatalogTypeDto& type) {
    const auto known = catalog::knownFeatureSlugs();
    const std::pair<std::uint16_t, QString> capSlugs[] = {
        {proto::kCapAnalogTriggers, catalog::kFeatureAnalogTriggers},
        {proto::kCapRumble, catalog::kFeatureRumble},
        {proto::kCapMotion, catalog::kFeatureMotion},
        {proto::kCapLightbar, catalog::kFeatureLightbar},
    };
    std::uint16_t allowed = detectedCaps;
    for (const auto& [bit, slug] : capSlugs) {
        if ((allowed & bit) != 0 && !isFeatureOffered(type, slug, known)) {
            allowed = static_cast<std::uint16_t>(allowed & ~bit);
        }
    }
    return allowed;
}

// Bridges the DTO into typeOffersDs4Touchpad so the mode rule keeps one owner.
inline bool typeOffersTouchpadDs4(const models::CatalogTypeDto& type) {
    const auto it = type.features.constFind(catalog::kFeatureTouchpad);
    if (it == type.features.constEnd()) { return false; }
    std::vector<std::string> modes;
    modes.reserve(static_cast<std::size_t>(it->modes.size()));
    for (const auto& mode : it->modes) { modes.push_back(mode.toStdString()); }
    return typeOffersDs4Touchpad(it->supported, modes);
}

// A type the satellite's catalog does not list renders nothing: the satellite
// refuses that type outright.
inline bool catalogTypeOffersTouchpadDs4(const models::CatalogDto& catalog, int typeId) {
    const auto& types = catalog.controllerTypes;
    const auto it = std::find_if(types.cbegin(), types.cend(),
                                 [typeId](const auto& type) { return type.id == typeId; });
    return it != types.cend() && typeOffersTouchpadDs4(*it);
}

// Every touchpad the bundled table lists renders the DS4 pad, the way the
// legacy catalog translation substitutes it.
inline bool bundledTypeOffersTouchpadDs4(int typeId) {
    return catalog::typeFeatureSlugsById(typeId).contains(catalog::kFeatureTouchpad);
}

// Whether the type a slot is bound as renders the DS4 pad: the satellite's own
// catalog once it is cached, the bundled table before it arrives.
inline bool boundTypeOffersTouchpadDs4(int typeId,
                                       const std::optional<models::CatalogDto>& catalog) {
    return catalog.has_value() ? catalogTypeOffersTouchpadDs4(*catalog, typeId)
                               : bundledTypeOffersTouchpadDs4(typeId);
}

// The touchpadMode a bound slot declares: the host's pick, or the out-of-box
// default, through the ds4 > mouse > off ladder, with the pad render gated on
// what the bound type renders.
inline std::uint8_t declaredTouchpadMode(const std::optional<std::string>& storedPick,
                                         bool padHasTouchpad, int typeId,
                                         const std::optional<models::CatalogDto>& catalog) {
    const std::string pick = touchpadPickOrDefault(storedPick);
    const bool typeOffersDs4 = boundTypeOffersTouchpadDs4(typeId, catalog);
    return resolveTouchpadMode(pick, padHasTouchpad, typeOffersDs4, /*hostMouseControl=*/false);
}

} // namespace dish::reducer
