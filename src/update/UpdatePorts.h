// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one seam between the update checker and the outside world: the manifest
// fetch. Abstract on purpose (the DisplaySleepInhibitor pattern) so the
// checker's schedule, gating and backoff are testable with no sockets.
//
// Dish never installs its own update on Linux — packages come from the distro
// or Flatpak — so there is no download gateway and no staging store. The
// checker surfaces the release and stops.
//
// Threading: the gateway is called only from the thread that owns it and every
// callback fires on that same thread.

#pragma once

#include "core/reducer/UpdateMachine.h"
#include "core/update/UpdateManifest.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>

namespace dish::update {

// Either a validated manifest or the typed reason the check failed. Never both.
struct ManifestFetchResult {
    std::optional<UpdateManifest> manifest;
    // The exact bytes that parsed, snapshotted beside a promoted stage so a
    // support request can show what the client was told at stage time.
    QByteArray body;
    reducer::UpdateError error = reducer::UpdateError::None;

    static ManifestFetchResult ok(UpdateManifest m, QByteArray raw) {
        return ManifestFetchResult{std::move(m), std::move(raw), reducer::UpdateError::None};
    }
    static ManifestFetchResult failed(reducer::UpdateError e) {
        return ManifestFetchResult{std::nullopt, {}, e};
    }
};

class ManifestGateway {
  public:
    using Callback = std::function<void(const ManifestFetchResult&)>;

    virtual ~ManifestGateway() = default;

    // Exactly one callback per accepted fetch. A fetch issued while one is in
    // flight is ignored (the coordinator's phase guard already prevents it).
    virtual void fetch(Callback done) = 0;

    // Best-effort; any in-flight callback is dropped, never delivered late.
    virtual void cancel() = 0;
};

} // namespace dish::update
