// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/ClientReaper.h"

#include "Network/SatelliteClient.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <utility>

namespace dish::net {

ClientReaper::ClientReaper() : thread_([this] { run(); }) {}

ClientReaper::~ClientReaper() {
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        stopping_ = true;
    }
    wake_.notify();
    thread_.join();
}

void ClientReaper::retire(std::shared_ptr<SatelliteClient> client) {
    if (!client) { return; }
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        queue_.push_back(std::move(client));
        ++retired_;
    }
    wake_.notify();
}

// Shutdown's, so a short poll is all it needs.
void ClientReaper::drain() {
    std::uint64_t target = 0;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        target = retired_;
    }
    while (closed_.load(std::memory_order_acquire) < target) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ClientReaper::run() {
    for (;;) {
        wake_.sleepUnless([this] {
            std::lock_guard<thread::TrackedMutex> lock(mtx_);
            return stopping_ || !queue_.empty();
        });
        std::vector<std::shared_ptr<SatelliteClient>> batch;
        bool stopping = false;
        {
            std::lock_guard<thread::TrackedMutex> lock(mtx_);
            batch.swap(queue_);
            stopping = stopping_;
        }
        // The batch's references go with it. A client another owner still holds is closed by
        // then, so its last owner has nothing left to join.
        for (const auto& client : batch) { client->closeSocket(); }
        const std::size_t count = batch.size();
        batch.clear();
        closed_.fetch_add(count, std::memory_order_release);
        if (stopping && count == 0) { return; }
    }
}

} // namespace dish::net
