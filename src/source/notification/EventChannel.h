// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// EventChannel<E> — a small bounded, fire-and-forget event channel. Unlike an
// Observable<S> it retains no value (no replay to late subscribers), and unlike
// a Qt signal it carries an explicit bounded buffer for when no collector is
// attached to drain it.
//
// The architecture kernel deliberately has no event-channel primitive, because a
// Source exposes state, not events; only the two rare event-emitting classes
// need one. This header is scoped to source/notification, NOT a kernel addition.
//
//   * push(e): delivered synchronously and NOT buffered when a subscriber is
//     attached; otherwise queued, dropping the OLDEST event once full. Named
//     push, not emit, because Qt reserves `emit` as a macro.
//   * subscribe(cb): attaches the single collector and drains the backlog to it
//     in FIFO order before future pushes go live.
//
// Single-collector. Not thread-safe; callers push on the UI loop.

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <utility>

namespace dish::source {

template <class E> class EventChannel {
  public:
    using Listener = std::function<void(const E&)>;

    explicit EventChannel(std::size_t capacity = 16) : capacity_(capacity == 0 ? 1 : capacity) {}

    void push(E event) {
        if (listener_) {
            listener_(event);
            return;
        }
        if (buffer_.size() >= capacity_) { buffer_.pop_front(); }
        buffer_.push_back(std::move(event));
    }

    // Replaces any prior listener.
    void subscribe(Listener listener) {
        listener_ = std::move(listener);
        if (!listener_) { return; }
        while (!buffer_.empty()) {
            E event = std::move(buffer_.front());
            buffer_.pop_front();
            listener_(event);
        }
    }

    // Subsequent pushes buffer again.
    void unsubscribe() { listener_ = nullptr; }

    std::size_t buffered() const { return buffer_.size(); }
    std::size_t capacity() const { return capacity_; }
    bool hasListener() const { return static_cast<bool>(listener_); }

  private:
    std::size_t capacity_;
    std::deque<E> buffer_;
    Listener listener_;
};

} // namespace dish::source
