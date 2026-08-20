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

#include <cstdint>
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

} // namespace dish::reducer
