// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// OutputCommandQueue marshals rumble / lightbar commands from the SatelliteClient
// receive thread onto the SDL thread. It carries no SDL dependency, so the
// marshalling is testable on a host with no controller attached.

#include "Input/OutputCommandQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using dish::input::OutputCommand;
using dish::input::OutputCommandQueue;
using dish::input::OutputKind;

TEST_CASE("OutputCommand::rumble carries the rumble payload", "[outputqueue]") {
    const auto cmd = OutputCommand::rumble(QStringLiteral("sdl:3"), 0xAABB, 0x1122, 250);
    REQUIRE(cmd.kind == OutputKind::Rumble);
    REQUIRE(cmd.deviceId == QStringLiteral("sdl:3"));
    REQUIRE(cmd.strongMagnitude == 0xAABB);
    REQUIRE(cmd.weakMagnitude == 0x1122);
    REQUIRE(cmd.durationMs == 250);
}

TEST_CASE("OutputCommand::lightbar carries the RGB payload", "[outputqueue]") {
    const auto cmd = OutputCommand::lightbar(QStringLiteral("sdl:7"), 0xDE, 0xAD, 0xBE);
    REQUIRE(cmd.kind == OutputKind::Lightbar);
    REQUIRE(cmd.deviceId == QStringLiteral("sdl:7"));
    REQUIRE(cmd.r == 0xDE);
    REQUIRE(cmd.g == 0xAD);
    REQUIRE(cmd.b == 0xBE);
}

TEST_CASE("a freshly-constructed queue is empty", "[outputqueue]") {
    OutputCommandQueue q;
    REQUIRE(q.size() == 0U);
    REQUIRE(q.drain().empty());
}

TEST_CASE("push then drain yields the queued command verbatim", "[outputqueue]") {
    OutputCommandQueue q;
    q.push(OutputCommand::lightbar(QStringLiteral("sdl:1"), 10, 20, 30));
    REQUIRE(q.size() == 1U);

    const auto batch = q.drain();
    REQUIRE(batch.size() == 1U);
    REQUIRE(batch[0].kind == OutputKind::Lightbar);
    REQUIRE(batch[0].deviceId == QStringLiteral("sdl:1"));
    REQUIRE(batch[0].r == 10);
    REQUIRE(batch[0].g == 20);
    REQUIRE(batch[0].b == 30);
}

TEST_CASE("drain empties the queue", "[outputqueue]") {
    OutputCommandQueue q;
    q.push(OutputCommand::rumble(QStringLiteral("sdl:0"), 1, 2, 3));
    REQUIRE(q.size() == 1U);
    (void)q.drain();
    REQUIRE(q.size() == 0U);
    REQUIRE(q.drain().empty());
}

TEST_CASE("the queue preserves FIFO order across mixed command kinds", "[outputqueue]") {
    OutputCommandQueue q;
    q.push(OutputCommand::rumble(QStringLiteral("sdl:0"), 100, 0, 80));
    q.push(OutputCommand::lightbar(QStringLiteral("sdl:0"), 1, 2, 3));
    q.push(OutputCommand::rumble(QStringLiteral("sdl:1"), 0, 0, 0));

    const auto batch = q.drain();
    REQUIRE(batch.size() == 3U);
    REQUIRE(batch[0].kind == OutputKind::Rumble);
    REQUIRE(batch[0].strongMagnitude == 100);
    REQUIRE(batch[1].kind == OutputKind::Lightbar);
    REQUIRE(batch[1].r == 1);
    REQUIRE(batch[2].kind == OutputKind::Rumble);
    REQUIRE(batch[2].deviceId == QStringLiteral("sdl:1"));
}

TEST_CASE("a rumble 'stop' (all-zero magnitudes) survives the round trip", "[outputqueue]") {
    // durationMs == 0 is the wire's "stop" signal and is forwarded to SDL
    // verbatim, so the queue must not drop or normalise it.
    OutputCommandQueue q;
    q.push(OutputCommand::rumble(QStringLiteral("sdl:2"), 0, 0, 0));
    const auto batch = q.drain();
    REQUIRE(batch.size() == 1U);
    REQUIRE(batch[0].kind == OutputKind::Rumble);
    REQUIRE(batch[0].strongMagnitude == 0);
    REQUIRE(batch[0].weakMagnitude == 0);
    REQUIRE(batch[0].durationMs == 0);
}

TEST_CASE("push from another thread is observed by a draining consumer",
          "[outputqueue][threading]") {
    // The real topology: the receive thread pushes, the SDL thread drains, and
    // the queue's own mutex is the only thing making that safe.
    OutputCommandQueue q;
    constexpr int kCount = 2000;

    std::atomic<bool> go{false};
    std::thread producer([&] {
        while (!go.load()) { /* spin until the consumer is ready */
        }
        for (int i = 0; i < kCount; ++i) {
            q.push(OutputCommand::lightbar(QStringLiteral("sdl:0"),
                                           static_cast<std::uint8_t>(i & 0xFF), 0, 0));
        }
    });

    go.store(true);
    std::vector<OutputCommand> collected;
    while (static_cast<int>(collected.size()) < kCount) {
        for (auto& cmd : q.drain()) { collected.push_back(cmd); }
    }
    producer.join();
    // Drain once more in case the producer raced in after the last drain.
    for (auto& cmd : q.drain()) { collected.push_back(cmd); }

    REQUIRE(collected.size() == static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(collected[static_cast<std::size_t>(i)].kind == OutputKind::Lightbar);
        REQUIRE(collected[static_cast<std::size_t>(i)].r == static_cast<std::uint8_t>(i & 0xFF));
    }
    REQUIRE(q.size() == 0U);
}
