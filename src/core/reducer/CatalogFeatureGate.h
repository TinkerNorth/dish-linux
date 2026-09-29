// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the satellite's catalog decides about a bound slot's touchpad: whether
// the type renders the DS4 pad, whether the host advertises mouse control, and
// so the touchpadMode the descriptor declares. The descriptor's caps word is not
// gated here: a cap advertises what this client can actuate on the physical pad
// (satellite docs/contract.md), not what the emulated type offers.

#pragma once

#include "Models/Models.h"
#include "core/catalog/BundledCatalog.h"
#include "core/reducer/TouchpadModeResolve.h"

#include <QString>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dish::reducer {

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

// Whether a satellite's catalog advertises host mouse control. The capability
// previews and the runtime both read it.
inline bool
hostAdvertisesMouseControl(const QHash<QString, models::CatalogHostFeatureDto>& hostFeatures) {
    const auto it = hostFeatures.constFind(catalog::kHostFeatureMouseControl);
    return it != hostFeatures.constEnd() && it->supported;
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
// what the bound type renders and the mouse on whether mouse mode is available.
inline std::uint8_t declaredTouchpadMode(const std::optional<std::string>& storedPick,
                                         bool padHasTouchpad, int typeId,
                                         const std::optional<models::CatalogDto>& catalog) {
    const std::string pick = touchpadPickOrDefault(storedPick);
    const bool typeOffersDs4 = boundTypeOffersTouchpadDs4(typeId, catalog);
    const bool hostAdvertisesMouse =
        catalog.has_value() && hostAdvertisesMouseControl(catalog->hostFeatures);
    return resolveTouchpadMode(pick, padHasTouchpad, typeOffersDs4,
                               mouseModeAvailable(hostAdvertisesMouse));
}

} // namespace dish::reducer
