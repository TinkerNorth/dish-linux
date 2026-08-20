// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/AsyncState.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using dish::core::asyncIdle;
using dish::core::AsyncPhase;
using dish::core::AsyncState;
using dish::core::toError;
using dish::core::toLoading;
using dish::core::toLoadingFresh;
using dish::core::toRevalidated;
using dish::core::toSuccess;

namespace {

// Stands in for the per-flow error codes real callers use (CatalogError,
// PairFailure), so the reason travels as data rather than as a string.
enum class FetchError { Unreachable, ServerError, Malformed };

using IntState = AsyncState<int, FetchError>;

} // namespace

TEST_CASE("asyncIdle is the empty starting point", "[async_state]") {
    const auto s = asyncIdle<int, FetchError>();
    REQUIRE(s.phase == AsyncPhase::Idle);
    REQUIRE(s.isIdle());
    REQUIRE_FALSE(s.hasData());
    REQUIRE_FALSE(s.error.has_value());
    REQUIRE_FALSE(s.stale);
}

TEST_CASE("toLoadingFresh enters Loading with no carried data", "[async_state]") {
    const auto s = toLoadingFresh<int, FetchError>();
    REQUIRE(s.isLoading());
    REQUIRE_FALSE(s.hasData());
    REQUIRE_FALSE(s.stale);
}

TEST_CASE("toLoading from Idle has no stale data", "[async_state]") {
    const auto s = toLoading(asyncIdle<int, FetchError>());
    REQUIRE(s.isLoading());
    REQUIRE_FALSE(s.hasData());
    REQUIRE_FALSE(s.stale);
}

TEST_CASE("toSuccess carries a confirmed value, not stale", "[async_state]") {
    const auto s = toSuccess(asyncIdle<int, FetchError>(), 42);
    REQUIRE(s.isSuccess());
    REQUIRE(s.hasData());
    REQUIRE(*s.data == 42);
    REQUIRE_FALSE(s.stale);
    REQUIRE_FALSE(s.error.has_value());
}

TEST_CASE("toLoading after a Success retains data as stale (stale-while-revalidate)",
          "[async_state]") {
    const auto loaded = toSuccess(asyncIdle<int, FetchError>(), 7);
    const auto refreshing = toLoading(loaded);
    REQUIRE(refreshing.isLoading());
    REQUIRE(refreshing.hasData()); // last-good value still shown
    REQUIRE(*refreshing.data == 7);
    REQUIRE(refreshing.stale); // but flagged not-fresh
    REQUIRE_FALSE(refreshing.error.has_value());
}

TEST_CASE("toRevalidated re-serves the prior value but stays flagged stale (HTTP 304)",
          "[async_state]") {
    const auto loaded = toSuccess(asyncIdle<int, FetchError>(), 9);
    const auto refreshing = toLoading(loaded);
    const auto revalidated = toRevalidated(refreshing);
    REQUIRE(revalidated.isSuccess());
    REQUIRE(*revalidated.data == 9);
    REQUIRE(revalidated.stale);
    REQUIRE_FALSE(revalidated.error.has_value());
}

TEST_CASE("toError from cold carries the reason and no data", "[async_state]") {
    const auto s = toError(asyncIdle<int, FetchError>(), FetchError::Unreachable);
    REQUIRE(s.isError());
    REQUIRE_FALSE(s.hasData());
    REQUIRE(s.error.has_value());
    REQUIRE(*s.error == FetchError::Unreachable);
    REQUIRE_FALSE(s.stale);
}

TEST_CASE("toError after a Success serves the cached value stale alongside the reason",
          "[async_state]") {
    const auto loaded = toSuccess(asyncIdle<int, FetchError>(), 5);
    const auto errored = toError(loaded, FetchError::ServerError);
    REQUIRE(errored.isError());
    REQUIRE(errored.hasData()); // cached content survives the failure
    REQUIRE(*errored.data == 5);
    REQUIRE(errored.stale);
    REQUIRE(*errored.error == FetchError::ServerError);
}

TEST_CASE("a fresh toSuccess clears a prior error and the stale flag", "[async_state]") {
    auto s = toError(toSuccess(asyncIdle<int, FetchError>(), 1), FetchError::Malformed);
    REQUIRE(s.isError());
    s = toSuccess(s, 2);
    REQUIRE(s.isSuccess());
    REQUIRE(*s.data == 2);
    REQUIRE_FALSE(s.error.has_value());
    REQUIRE_FALSE(s.stale);
}

TEST_CASE("valueOr returns data when present, fallback otherwise", "[async_state]") {
    REQUIRE(asyncIdle<int, FetchError>().valueOr(99) == 99);
    REQUIRE(toSuccess(asyncIdle<int, FetchError>(), 3).valueOr(99) == 3);
}

TEST_CASE("equality distinguishes phase, data, error, and stale", "[async_state]") {
    const auto a = toSuccess(asyncIdle<int, FetchError>(), 1);
    const auto b = toSuccess(asyncIdle<int, FetchError>(), 1);
    REQUIRE(a == b); // identical Success — distinct-until-changed suppresses a re-emit

    REQUIRE(a != toSuccess(asyncIdle<int, FetchError>(), 2)); // data differs
    REQUIRE(a != toLoading(a));                               // phase differs (+stale)
    REQUIRE(toError(a, FetchError::Unreachable) !=
            toError(a, FetchError::ServerError)); // reason differs

    // Same phase + same data but different stale flag must compare unequal so the
    // UI can react to "fresh" vs "served-stale".
    IntState freshSuccess = toSuccess(asyncIdle<int, FetchError>(), 4);
    IntState staleSuccess = toRevalidated(toLoading(freshSuccess));
    REQUIRE(freshSuccess != staleSuccess);
}

TEST_CASE("works over a container value type (the common case: a list)", "[async_state]") {
    using ListState = AsyncState<std::vector<int>, FetchError>;
    ListState s = dish::core::toSuccess(ListState{}, std::vector<int>{});
    REQUIRE(s.isSuccess());
    REQUIRE(s.hasData());
    REQUIRE(
        s.data->empty()); // Success-with-empty-list — the "Empty" UI case, distinct from Loading
}
