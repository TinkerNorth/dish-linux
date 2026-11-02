// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/SatelliteClient.h"
#include "Network/WifiConnection.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using dish::net::ClientRef;
using dish::net::SatelliteClient;

TEST_CASE("a replaced client is destroyed after the reference lets go of its lock",
          "[client-ref]") {
    using namespace std::chrono_literals;
    constexpr auto kSlowDestructor = 300ms;
    ClientRef ref;
    std::atomic<bool> destroying{false};
    ref.set(std::shared_ptr<SatelliteClient>(new SatelliteClient, [&](SatelliteClient* client) {
        destroying.store(true);
        std::this_thread::sleep_for(kSlowDestructor);
        delete client;
    }));

    std::thread replacer([&ref] { ref.set(nullptr); });
    while (!destroying.load()) { std::this_thread::yield(); }
    const auto asked = std::chrono::steady_clock::now();
    const auto current = ref.get();
    const auto waited = std::chrono::steady_clock::now() - asked;
    replacer.join();

    CHECK(current == nullptr);
    CHECK(waited < kSlowDestructor / 3);
}
