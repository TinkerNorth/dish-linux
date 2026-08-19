// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A single async value that is one of { Idle, Loading, Success, Error }, so a
// component can bind all four instead of guessing from a bool and an optional.
// Stale-while-revalidate is built in: toLoading/toError keep the prior data and
// flag it `stale`, so a refresh never blanks the screen. `T` and `E` must be
// equality-comparable to get distinct-until-changed inside an Observable.
// Prefer a typed enum for `E` so the UI localizes the reason from data.

#pragma once

#include <optional>
#include <string>
#include <utility>

namespace dish::core {

enum class AsyncPhase { Idle, Loading, Success, Error };

template <class T, class E = std::string> struct AsyncState {
    AsyncPhase phase = AsyncPhase::Idle;
    std::optional<T> data;  // last good value; retained across Loading and Error
    std::optional<E> error; // set iff phase == Error
    bool stale = false;     // `data` carried from a prior success, not confirmed this cycle

    bool isIdle() const { return phase == AsyncPhase::Idle; }
    bool isLoading() const { return phase == AsyncPhase::Loading; }
    bool isSuccess() const { return phase == AsyncPhase::Success; }
    bool isError() const { return phase == AsyncPhase::Error; }
    bool hasData() const { return data.has_value(); }

    const T& valueOr(const T& fallback) const { return data ? *data : fallback; }

    bool operator==(const AsyncState& o) const {
        return phase == o.phase && data == o.data && error == o.error && stale == o.stale;
    }
    bool operator!=(const AsyncState& o) const { return !(*this == o); }
};

// ── Pure transitions ─────────────────────────────────────────────────────────

template <class T, class E = std::string> AsyncState<T, E> asyncIdle() {
    return AsyncState<T, E>{};
}

// Keeps prior data (marked stale) so a refresh overlays a spinner on the last
// good content rather than blanking it.
template <class T, class E> AsyncState<T, E> toLoading(const AsyncState<T, E>& prev) {
    AsyncState<T, E> next;
    next.phase = AsyncPhase::Loading;
    next.data = prev.data;
    next.error = std::nullopt;
    next.stale = prev.data.has_value();
    return next;
}

// Drops prior data: for an operation whose new run invalidates the old value,
// such as a fresh discovery scan.
template <class T, class E = std::string> AsyncState<T, E> toLoadingFresh() {
    AsyncState<T, E> next;
    next.phase = AsyncPhase::Loading;
    return next;
}

template <class T, class E> AsyncState<T, E> toSuccess(const AsyncState<T, E>& /*prev*/, T value) {
    AsyncState<T, E> next;
    next.phase = AsyncPhase::Success;
    next.data = std::move(value);
    next.error = std::nullopt;
    next.stale = false;
    return next;
}

// Re-serving a cached value (an HTTP 304, say): Success, but `stale` stays true
// because the data was not re-fetched.
template <class T, class E> AsyncState<T, E> toRevalidated(const AsyncState<T, E>& prev) {
    AsyncState<T, E> next;
    next.phase = AsyncPhase::Success;
    next.data = prev.data;
    next.error = std::nullopt;
    next.stale = true;
    return next;
}

// Keeps prior data so the UI can show "couldn't refresh, showing last known"
// instead of going blank.
template <class T, class E> AsyncState<T, E> toError(const AsyncState<T, E>& prev, E reason) {
    AsyncState<T, E> next;
    next.phase = AsyncPhase::Error;
    next.data = prev.data;
    next.error = std::move(reason);
    next.stale = prev.data.has_value();
    return next;
}

} // namespace dish::core
