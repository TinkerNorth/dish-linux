// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The ETag caching gateway, driven by a fake Fetch that pops canned CatalogDto
// replies and records the conditional ETag it was handed. No real socket.

#include "Models/Models.h"
#include "source/http/SatelliteCatalogRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QString>

#include <deque>
#include <optional>

using dish::models::CatalogDto;
using dish::models::CatalogTypeDto;
using dish::models::DiscoveredServer;
using dish::source::SatelliteCatalogRepository;

namespace {

// The shapes HTTPClient::getCatalog delivers. The cache treats an empty
// serverVersion (what fromJson defaults a blank/malformed body to) as not-a-fill.
CatalogDto reply200(const QString& etag, const QString& slug = QStringLiteral("xbox360")) {
    CatalogDto c;
    c.httpStatus = 200;
    c.reachable = true;
    c.notModified = false;
    c.etag = etag;
    c.serverVersion = "1.6.0";
    // Current schema, so the body passes through: a legacy version would have its
    // controllerTypes substituted at the fill boundary (its own case, below).
    c.catalogVersion = 2;
    CatalogTypeDto t;
    t.id = 0;
    t.slug = slug;
    t.name = "Xbox 360 Controller";
    c.controllerTypes.push_back(t);
    return c;
}

CatalogDto reply304() {
    CatalogDto c;
    c.httpStatus = 304;
    c.reachable = true;
    c.notModified = true;
    return c;
}

CatalogDto replyServerError() {
    CatalogDto c;
    c.httpStatus = 500;
    c.reachable = true;
    c.serverVersion = ""; // an error body carries no serverVersion
    return c;
}

CatalogDto replyMalformed200() {
    CatalogDto c;
    c.httpStatus = 200;
    c.reachable = true;
    c.serverVersion = ""; // "not json" parses to a defaulted DTO
    return c;
}

CatalogDto replyUnreachable() {
    CatalogDto c;
    c.httpStatus = 0;
    c.reachable = false;
    return c;
}

// An empty recorded etag means the GET went out unconditional.
struct FakeFetch {
    std::deque<CatalogDto> replies;
    QList<QString> receivedEtags;
    bool throwInstead = false; // make every fetch fail outright

    SatelliteCatalogRepository::Fetch fn() {
        return [this](const QString&, int, const QString&, const QString& etag,
                      SatelliteCatalogRepository::Done done) {
            receivedEtags.push_back(etag);
            if (throwInstead || replies.empty()) {
                done(replyUnreachable());
                return;
            }
            CatalogDto next = replies.front();
            replies.pop_front();
            done(next);
        };
    }
};

DiscoveredServer makeServer() {
    DiscoveredServer s;
    s.name = "Pc";
    s.ip = "10.0.0.5";
    s.udpPort = 9876;
    s.machineId = "m1";
    return s;
}

dish::source::CatalogState fetchSync(SatelliteCatalogRepository& repo, const DiscoveredServer& s,
                                     const QString& satId) {
    dish::source::CatalogState captured;
    bool fired = false;
    repo.catalogFor(s, satId, QStringLiteral("en"), [&](const dish::source::CatalogState& r) {
        captured = r;
        fired = true;
    });
    REQUIRE(fired); // the fake fetch is synchronous
    return captured;
}

} // namespace

using dish::source::CatalogError;

TEST_CASE("CatalogCache: a 200 fills the cache and the next fetch revalidates with the stored ETag",
          "[catalog][cache]") {
    FakeFetch fake;
    fake.replies.push_back(reply200(QStringLiteral("\"1.6.0\"")));
    fake.replies.push_back(reply304());
    SatelliteCatalogRepository repo(fake.fn());
    const auto server = makeServer();

    const auto first = fetchSync(repo, server, "sat-1");
    REQUIRE(first.isSuccess());
    REQUIRE_FALSE(first.stale);
    REQUIRE(first.hasData());
    REQUIRE(first.data->controllerTypes.size() == 1);
    REQUIRE(first.data->controllerTypes.front().slug == "xbox360");
    REQUIRE(fake.receivedEtags[0].isEmpty()); // nothing cached yet

    const auto second = fetchSync(repo, server, "sat-1");
    REQUIRE(fake.receivedEtags[1] == "\"1.6.0\""); // If-None-Match from the cache
    REQUIRE(second.isSuccess());
    REQUIRE(second.stale); // re-served from cache, so not freshly fetched
    REQUIRE(second.data->controllerTypes.front().slug == "xbox360");

    const auto cached = repo.cached("sat-1");
    REQUIRE(cached.has_value());
    REQUIRE(cached->controllerTypes.front().slug == "xbox360");
}

TEST_CASE("CatalogCache: a transport failure serves the last good copy", "[catalog][cache]") {
    FakeFetch fake;
    fake.replies.push_back(reply200(QString())); // first fill, no etag
    SatelliteCatalogRepository repo(fake.fn());
    const auto server = makeServer();

    REQUIRE(fetchSync(repo, server, "sat-1").isSuccess());

    fake.throwInstead = true;
    const auto stale = fetchSync(repo, server, "sat-1");
    REQUIRE(stale.isError());
    REQUIRE(*stale.error == CatalogError::Unreachable);
    REQUIRE(stale.hasData()); // the last good copy survives the error
    REQUIRE(stale.stale);
    REQUIRE(stale.data->controllerTypes.front().slug == "xbox360");
}

TEST_CASE("CatalogCache: a server error keeps the cache, a malformed body too",
          "[catalog][cache]") {
    FakeFetch fake;
    fake.replies.push_back(reply200(QString()));
    fake.replies.push_back(replyServerError());
    fake.replies.push_back(replyMalformed200());
    SatelliteCatalogRepository repo(fake.fn());
    const auto server = makeServer();

    REQUIRE(fetchSync(repo, server, "sat-1").isSuccess());

    const auto afterServerError = fetchSync(repo, server, "sat-1");
    REQUIRE(afterServerError.isError());
    REQUIRE(*afterServerError.error == CatalogError::ServerError);
    REQUIRE(afterServerError.hasData());
    REQUIRE(afterServerError.data->controllerTypes.front().slug == "xbox360");

    const auto afterMalformed = fetchSync(repo, server, "sat-1");
    REQUIRE(afterMalformed.isError());
    REQUIRE(*afterMalformed.error == CatalogError::Malformed);
    REQUIRE(afterMalformed.hasData());
    REQUIRE(afterMalformed.data->controllerTypes.front().slug == "xbox360");
}

TEST_CASE("CatalogCache: a never-reachable satellite yields nullopt, no cache to serve",
          "[catalog][cache]") {
    FakeFetch fake;
    fake.throwInstead = true;
    SatelliteCatalogRepository repo(fake.fn());
    const auto server = makeServer();

    const auto cold = fetchSync(repo, server, "sat-1");
    REQUIRE(cold.isError());
    REQUIRE(*cold.error == CatalogError::Unreachable);
    REQUIRE_FALSE(cold.hasData());
    REQUIRE_FALSE(repo.cached("sat-1").has_value());
}

TEST_CASE("CatalogCache: the cache is keyed per satellite id", "[catalog][cache]") {
    FakeFetch fake;
    fake.replies.push_back(reply200(QStringLiteral("\"1.6.0\""), QStringLiteral("xbox360")));
    fake.replies.push_back(reply200(QStringLiteral("\"1.7.0\""), QStringLiteral("ds4")));
    SatelliteCatalogRepository repo(fake.fn());
    const auto server = makeServer();

    const auto a = fetchSync(repo, server, "sat-A");
    const auto b = fetchSync(repo, server, "sat-B");
    REQUIRE(a.data->controllerTypes.front().slug == "xbox360");
    REQUIRE(b.data->controllerTypes.front().slug == "ds4");
    // Neither first fetch is conditional: each satellite starts with a cold slot.
    REQUIRE(fake.receivedEtags[0].isEmpty());
    REQUIRE(fake.receivedEtags[1].isEmpty());
    REQUIRE(repo.cached("sat-A")->controllerTypes.front().slug == "xbox360");
    REQUIRE(repo.cached("sat-B")->controllerTypes.front().slug == "ds4");
}

TEST_CASE("CatalogCache: a legacy (absent-version) body is substituted at the fill boundary",
          "[catalog][cache][legacy]") {
    // A response omitting catalogVersion is the legacy v1 catalog, which a client
    // may substitute. The translator runs BEFORE caching, so every caller sees
    // the canonical xbox360+ds4 shape while the server's ETag still keys
    // revalidation.
    FakeFetch fake;
    auto legacy = reply200(QStringLiteral("\"1.2.0\""), QStringLiteral("weird-legacy-slug"));
    legacy.catalogVersion = 1; // absent parses as 1 — the legacy tell
    fake.replies.push_back(legacy);
    SatelliteCatalogRepository repo(fake.fn());

    const auto got = fetchSync(repo, makeServer(), "sat-legacy");
    REQUIRE(got.isSuccess());
    REQUIRE(got.data->controllerTypes.size() == 2);
    CHECK(got.data->controllerTypes[0].slug == "xbox360");
    CHECK(got.data->controllerTypes[1].slug == "ds4");
    const auto& touchpad = got.data->controllerTypes[1].features.value("touchpad");
    CHECK(touchpad.supported);
    CHECK(touchpad.modes.contains(QStringLiteral("ds4")));
    REQUIRE(repo.cached("sat-legacy").has_value());
    CHECK(repo.cached("sat-legacy")->controllerTypes.size() == 2);
}
