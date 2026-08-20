// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The property tests every concrete Repository<K,V> must pass. Call from a
// TEST_CASE; Catch2 re-runs the body per SECTION, so makeRepo() must hand back
// a fresh empty repository each time.

#pragma once

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace dish::test {

template <class K, class V, class MakeRepo, class KeyFn, class ValueFn>
void runRepositoryContract(MakeRepo makeRepo, KeyFn key, ValueFn value) {
    const auto contains = [](const std::vector<V>& all, const V& v) {
        return std::find(all.begin(), all.end(), v) != all.end();
    };

    SECTION("empty get returns nothing and empty all is empty") {
        auto repo = makeRepo();
        REQUIRE_FALSE(repo->get(key(0)).has_value());
        REQUIRE(repo->all().empty());
    }
    SECTION("put then get returns the value") {
        auto repo = makeRepo();
        const K k = key(0);
        const V v = value(k);
        repo->put(k, v);
        const auto got = repo->get(k);
        REQUIRE(got.has_value());
        REQUIRE(*got == v);
    }
    SECTION("all contains every put") {
        auto repo = makeRepo();
        const V v0 = value(key(0));
        const V v1 = value(key(1));
        const V v2 = value(key(2));
        repo->put(key(0), v0);
        repo->put(key(1), v1);
        repo->put(key(2), v2);
        const auto all = repo->all();
        REQUIRE(all.size() == 3);
        REQUIRE(contains(all, v0));
        REQUIRE(contains(all, v1));
        REQUIRE(contains(all, v2));
    }
    SECTION("replacing the same key overwrites and does not grow") {
        auto repo = makeRepo();
        const K k = key(0);
        repo->put(k, value(key(0)));
        const V replacement = value(key(1));
        repo->put(k, replacement);
        REQUIRE(*repo->get(k) == replacement);
        REQUIRE(repo->all().size() == 1);
    }
    SECTION("remove deletes the entry") {
        auto repo = makeRepo();
        const K k = key(0);
        repo->put(k, value(k));
        repo->remove(k);
        REQUIRE_FALSE(repo->get(k).has_value());
        REQUIRE(repo->all().empty());
    }
    SECTION("clear empties the repository") {
        auto repo = makeRepo();
        repo->put(key(0), value(key(0)));
        repo->put(key(1), value(key(1)));
        repo->clear();
        REQUIRE(repo->all().empty());
    }
    SECTION("removing an absent key is a no-op") {
        auto repo = makeRepo();
        const K k = key(0);
        repo->put(k, value(k));
        repo->remove(key(9));
        REQUIRE(repo->get(k).has_value());
        REQUIRE(repo->all().size() == 1);
    }
    SECTION("removing one key leaves the others") {
        auto repo = makeRepo();
        repo->put(key(0), value(key(0)));
        repo->put(key(1), value(key(1)));
        repo->remove(key(0));
        REQUIRE_FALSE(repo->get(key(0)).has_value());
        REQUIRE(repo->get(key(1)).has_value());
        REQUIRE(repo->all().size() == 1);
    }
}

} // namespace dish::test
