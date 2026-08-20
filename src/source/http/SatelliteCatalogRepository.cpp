// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/http/SatelliteCatalogRepository.h"

#include "core/catalog/LegacyCatalogTranslator.h"

#include "Network/HTTPClient.h"

#include <utility>

namespace dish::source {

namespace {

// The contract guarantees a 200 catalog carries a non-empty `serverVersion`, so
// an empty one is the tell for a malformed or blank body: getCatalog already ran
// fromJson, and a garbage body yields a default-constructed DTO.
bool isValidFill(const models::CatalogDto& reply) {
    return reply.httpStatus == 200 && reply.reachable && !reply.serverVersion.isEmpty();
}

CatalogError classifyCatalogError(const models::CatalogDto& reply) {
    if (!reply.reachable) { return CatalogError::Unreachable; }
    if (reply.httpStatus != 200) { return CatalogError::ServerError; }
    // reachable + 200 but not a valid fill ⇒ the body didn't parse into a catalog.
    return CatalogError::Malformed;
}

} // namespace

SatelliteCatalogRepository::SatelliteCatalogRepository(Fetch fetch) : fetch_(std::move(fetch)) {}

SatelliteCatalogRepository::SatelliteCatalogRepository(net::HTTPClient* http)
    : fetch_([http](const QString& ip, int httpPort, const QString& acceptLanguage,
                    const QString& etag, Done done) {
          http->getCatalog(ip, httpPort, acceptLanguage, etag, std::move(done));
      }) {}

void SatelliteCatalogRepository::catalogFor(const models::DiscoveredServer& server,
                                            const QString& satelliteId,
                                            const QString& acceptLanguage, CatalogCb cb) {
    QString conditionalEtag;
    std::optional<models::CatalogDto> cachedCatalog;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(satelliteId);
        if (it != cache_.end()) {
            conditionalEtag = it->second.etag;
            cachedCatalog = it->second.catalog;
        }
    }

    fetch_(server.ip, server.httpPort, acceptLanguage, conditionalEtag,
           [this, satelliteId, cachedCatalog, cb = std::move(cb)](const models::CatalogDto& reply) {
               // The pre-fetch baseline. The AsyncState transitions retain this
               // `data` (marked stale) across a revalidate or failure, which is
               // what stops the picker blanking.
               CatalogState prev = cachedCatalog
                                       ? core::toSuccess(CatalogState{}, *cachedCatalog)
                                       : core::asyncIdle<models::CatalogDto, CatalogError>();
               // A 304 with nothing cached is anomalous — the server claimed our
               // cache is good when we have none — so it reads as ServerError.
               if (reply.notModified) {
                   cb(cachedCatalog ? core::toRevalidated(prev)
                                    : core::toError(prev, CatalogError::ServerError));
                   return;
               }
               // Error still carries the last good copy (stale-on-error), so the
               // UI shows cached content beside the error chip.
               if (!isValidFill(reply)) {
                   cb(core::toError(prev, classifyCatalogError(reply)));
                   return;
               }
               // Normalize BEFORE caching so one schema reaches the cache and
               // every caller. The SERVER's ETag still keys revalidation —
               // normalization is client-local and must not feed back into it.
               const models::CatalogDto normalized = catalog::normalizeCatalog(reply);
               {
                   std::lock_guard<std::mutex> lock(mutex_);
                   cache_[satelliteId] = CacheEntry{reply.etag, normalized};
               }
               cb(core::toSuccess(prev, normalized));
           });
}

std::optional<models::CatalogDto>
SatelliteCatalogRepository::cached(const QString& satelliteId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(satelliteId);
    if (it == cache_.end()) { return std::nullopt; }
    return it->second.catalog;
}

} // namespace dish::source
