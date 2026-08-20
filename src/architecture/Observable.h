// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Observable<S>: hot value holder, distinct-until-changed via operator==.
// See architecture/README.md.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dish::arch {

template <class S> class Observable {
    struct Impl {
        explicit Impl(S v) : value(std::move(v)) {}
        mutable std::mutex mutex;
        S value;
        std::uint64_t nextToken = 1; // 0 is reserved for "no subscription"
        std::map<std::uint64_t, std::function<void(const S&)>> subscribers;
    };

  public:
    // Safe whether it outlives or predeceases the Observable: weak ref only.
    class Subscription {
      public:
        Subscription() = default;
        Subscription(std::weak_ptr<Impl> impl, std::uint64_t token)
            : impl_(std::move(impl)), token_(token) {}
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept { moveFrom(other); }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                moveFrom(other);
            }
            return *this;
        }
        ~Subscription() { reset(); }

        void reset() {
            if (token_ != 0) {
                if (auto impl = impl_.lock()) {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->subscribers.erase(token_);
                }
            }
            impl_.reset();
            token_ = 0;
        }

      private:
        void moveFrom(Subscription& other) {
            impl_ = std::move(other.impl_);
            token_ = other.token_;
            other.impl_.reset();
            other.token_ = 0;
        }
        std::weak_ptr<Impl> impl_;
        std::uint64_t token_ = 0;
    };

    explicit Observable(S initial) : impl_(std::make_shared<Impl>(std::move(initial))) {}

    Observable(const Observable&) = delete;
    Observable& operator=(const Observable&) = delete;
    Observable(Observable&&) noexcept = default;
    Observable& operator=(Observable&&) noexcept = default;
    ~Observable() = default;

    S value() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->value;
    }

    // Subscribers are invoked outside the lock so a callback may re-enter.
    void set(S next) {
        std::vector<std::function<void(const S&)>> toNotify;
        S snapshot;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->value == next) { return; }
            impl_->value = std::move(next);
            snapshot = impl_->value;
            toNotify.reserve(impl_->subscribers.size());
            for (const auto& entry : impl_->subscribers) { toNotify.push_back(entry.second); }
        }
        for (const auto& callback : toNotify) { callback(snapshot); }
    }

    // The reducer runs outside the lock.
    void update(const std::function<S(const S&)>& reducer) { set(reducer(value())); }

    // const so a Controller holding a `const Observable<S>&` can still subscribe.
    Subscription subscribe(std::function<void(const S&)> callback, bool emitCurrent = true) const {
        std::uint64_t token = 0;
        S snapshot;
        std::function<void(const S&)> immediate = callback;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            token = impl_->nextToken++;
            impl_->subscribers.emplace(token, std::move(callback));
            snapshot = impl_->value;
        }
        if (emitCurrent) { immediate(snapshot); }
        return Subscription(impl_, token);
    }

  private:
    std::shared_ptr<Impl> impl_;
};

} // namespace dish::arch
