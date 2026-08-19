// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SatelliteCatalogRepository — the per-satellite controller-type catalog cache
// (GET /api/catalog). The "Emulate" picker renders from this instead of a
// hardcoded enum, so a controller type newer than the app still gets a name and
// description from the server.
//
// The catalog is STATIC per server version + locale, so it is fetched once and
// revalidated by ETag: 200 fills the cache, 304 serves the cached copy, and a
// transport/server/malformed failure serves the last good copy (stale-on-error)
// rather than blanking the picker.
//
// Despite the name this is a caching GATEWAY, not a durable Repository: it owns
// IO plus an in-memory ETag cache and does not implement RepositoryContract.
// The fetch is injected as a `Fetch` callable so the cache/revalidate policy is
// testable with a fake transport.
//
// Version normalization happens HERE at the parse boundary (200 fill and
// stale-serve alike), so the cache and every caller see one shape and nothing
// downstream branches on catalogVersion.

#pragma once

#include "Models/Models.h"
#include "core/AsyncState.h"

#include <QString>

#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace dish::net {
class HTTPClient;
}

namespace dish::source {

// Carried AS DATA (the UI localizes it) so the picker can show a distinct
// message and retry per cause instead of one generic "try again".
enum class CatalogError {
    Unreachable, // transport never produced a response (offline / wrong host)
    ServerError, // a reply arrived but not a 200 catalog (5xx, etc.)
    Malformed,   // a 200 whose body did not parse into a real catalog
};

using CatalogState = core::AsyncState<models::CatalogDto, CatalogError>;

class SatelliteCatalogRepository {
  public:
    // The fetch seam: GET /api/catalog against `ip:httpPort` with the given
    // Accept-Language chain and stored ETag (empty = unconditional GET). The
    // reply carries httpStatus / notModified / reachable / etag.
    using Done = std::function<void(const models::CatalogDto&)>;
    using Fetch = std::function<void(const QString& ip, int httpPort, const QString& acceptLanguage,
                                     const QString& etag, Done done)>;

    explicit SatelliteCatalogRepository(Fetch fetch);
    // `http` must outlive this object (the composition root owns both).
    explicit SatelliteCatalogRepository(net::HTTPClient* http);

    using CatalogCb = std::function<void(const CatalogState&)>;

    // Delivers Success on a fresh 200, Success(stale) on a 304, and
    // Error(reason) otherwise — the Error STILL carries the last good catalog,
    // and carries no data only when the satellite was never reachable.
    // `acceptLanguage` is the locale chain. `cb` fires on the fetch's thread.
    void catalogFor(const models::DiscoveredServer& server, const QString& satelliteId,
                    const QString& acceptLanguage, CatalogCb cb);

    // Synchronous peek; never triggers a fetch.
    std::optional<models::CatalogDto> cached(const QString& satelliteId) const;

  private:
    struct CacheEntry {
        QString etag;
        models::CatalogDto catalog;
    };

    Fetch fetch_;
    mutable std::mutex mutex_;
    std::map<QString, CacheEntry> cache_;
};

} // namespace dish::source
