// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Offline per-slug capability sets for the controller types this client ships
// art and translations for: the fallback before any server catalog is fetched,
// and the feature source LegacyCatalogTranslator rebuilds legacy catalogs from.
// Unknown slugs return nullopt so a richer remote type is never masked by a stale
// bundled guess. Qt containers appear here because the CatalogTypeDto vocabulary
// is the data being produced; core/catalog uses Qt only where the DTOs force it.

#pragma once

#include "core/model/Protocol.h"

#include <QLatin1StringView>
#include <QString>
#include <QStringList>

#include <optional>

namespace dish::catalog {

// The bundled type slugs, in wire-id order (proto::kControllerType*).
inline constexpr QLatin1StringView kSlugXbox360("xbox360");
inline constexpr QLatin1StringView kSlugDs4("ds4");
inline constexpr QLatin1StringView kSlugDualSense("dualsense");
inline constexpr QLatin1StringView kSlugSwitchPro("switchpro");

// Catalog feature-slug vocabulary (protocol constants, never localized).
inline constexpr QLatin1StringView kFeatureAnalogTriggers("analogTriggers");
inline constexpr QLatin1StringView kFeatureRumble("rumble");
inline constexpr QLatin1StringView kFeatureMotion("motion");
inline constexpr QLatin1StringView kFeatureLightbar("lightbar");
inline constexpr QLatin1StringView kFeatureTouchpad("touchpad");
// Controller audio (protocol 2). Deliberately NOT in knownFeatureSlugs():
// that list is the protocol-1 vocabulary, and the audio slugs follow the
// trigger-effects precedent of a whitelist of their own. The capability
// solver's type layer reads these two directly.
inline constexpr QLatin1StringView kFeatureMic("mic");
inline constexpr QLatin1StringView kFeatureSpeaker("speaker");
// Protocol 3: the DualSense's HD-haptics lanes. Reads through the same audio
// gate as the two above; the solver has no row for it, since it rides the
// speaker toggle and the speaker route's endpoint.
inline constexpr QLatin1StringView kFeatureHapticAudio("hapticAudio");
// The protocol-2 feedback surfaces a live catalog reports per type. Also
// outside knownFeatureSlugs(), and deliberately absent from typeFeatureSlugs()
// below: a satellite old enough to serve no catalog predates the messages
// that carry them, so the legacy translation must not claim them.
inline constexpr QLatin1StringView kFeatureTriggerEffects("triggerEffects");
inline constexpr QLatin1StringView kFeaturePlayerLeds("playerLeds");

// The catalog's hostFeatures slug for touchpad-driven host mouse control.
inline constexpr QLatin1StringView kHostFeatureMouseControl("mouseControl");

// The `known` whitelist reducer::isFeatureOffered gates on, owned here so every
// caller passes the same vocabulary instead of re-listing it.
inline QStringList knownFeatureSlugs() {
    return {kFeatureRumble, kFeatureAnalogTriggers, kFeatureMotion, kFeatureLightbar,
            kFeatureTouchpad};
}

// The whitelist for the solver's audio type-layer reads, so the same
// isFeatureOffered gate serves them without widening the protocol-1 list.
inline QStringList audioFeatureSlugs() {
    return {kFeatureMic, kFeatureSpeaker, kFeatureHapticAudio};
}

// The same for the solver's feedback type-layer reads.
inline QStringList feedbackFeatureSlugs() { return {kFeatureTriggerEffects, kFeaturePlayerLeds}; }

// Order is fixed (triggers, rumble, extras) so the list is ==-comparable in tests
// and downstream snapshots.
//
// Audio rides the two Sony types only: they are the pads that carry real
// speaker and microphone endpoints, so they are the only identities a host can
// materialize with any. Offering them here cannot outrun the host, which gates
// audio on its own runtime controllerAudio switch, and a satellite old enough
// to serve no catalog reports no switch at all (mirrors dish-android's
// BundledCatalog).
inline std::optional<QStringList> typeFeatureSlugs(const QString& slug) {
    QStringList base{kFeatureAnalogTriggers, kFeatureRumble};
    if (slug == kSlugXbox360) { return base; }
    if (slug == kSlugDs4) {
        return base + QStringList{kFeatureMotion, kFeatureTouchpad, kFeatureLightbar, kFeatureMic,
                                  kFeatureSpeaker};
    }
    if (slug == kSlugDualSense) {
        return base + QStringList{kFeatureMotion, kFeatureTouchpad, kFeatureLightbar,
                                  kFeatureMic,    kFeatureSpeaker,  kFeatureHapticAudio};
    }
    if (slug == kSlugSwitchPro) { return base + QStringList{kFeatureMotion}; }
    return std::nullopt;
}

// An id outside the bundled range degrades to the xbox360 set, the least-capable
// baseline, rather than claiming features an unknown type may not have. The
// same floor covers a slug that stops resolving: value_or, not a dereference,
// so a future edit to typeFeatureSlugs degrades instead of being undefined.
inline QStringList typeFeatureSlugsById(int typeId) {
    QStringList floor = typeFeatureSlugs(kSlugXbox360).value_or(QStringList{});
    switch (typeId) {
    case proto::kControllerTypePlayStation:
        return typeFeatureSlugs(kSlugDs4).value_or(floor);
    case proto::kControllerTypeDualSense:
        return typeFeatureSlugs(kSlugDualSense).value_or(floor);
    case proto::kControllerTypeSwitchPro:
        return typeFeatureSlugs(kSlugSwitchPro).value_or(floor);
    default:
        return floor;
    }
}

} // namespace dish::catalog
