// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SIGUSR1/SIGUSR2 rather than SIGTERM: their default disposition also kills the
// process, so every raise() here is preceded by an asserted-successful install.

#include "Util/UnixSignalWatcher.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <vector>

using dish::util::installSignalWatcher;
using dish::util::UnixSignalWatcher;

namespace {

// QSignalSpy stand-in: DishTests links Catch2, not Qt6::Test.
struct NumberSpy {
    std::vector<int> seen;

    explicit NumberSpy(UnixSignalWatcher* watcher) {
        QObject::connect(watcher, &UnixSignalWatcher::signalled,
                         [this](int number) { seen.push_back(number); });
    }
};

// The notifier only fires from an event loop, so every expectation has to be
// pumped rather than merely awaited.
bool pumpUntil(const NumberSpy& spy, std::size_t wanted) {
    QEventLoop loop;
    QElapsedTimer deadline;
    deadline.start();
    while (spy.seen.size() < wanted && !deadline.hasExpired(2000)) {
        loop.processEvents(QEventLoop::AllEvents, 20);
    }
    return spy.seen.size() >= wanted;
}

bool dispositionIsDefault(int number) {
    struct sigaction current{};
    ::sigaction(number, nullptr, &current);
    return current.sa_handler == SIG_DFL;
}

} // namespace

TEST_CASE("a byte on the pipe becomes a Qt signal carrying the number") {
    int fds[2] = {-1, -1};
    REQUIRE(::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);

    UnixSignalWatcher watcher(fds[0]);
    NumberSpy spy(&watcher);

    const auto byte = static_cast<unsigned char>(SIGTERM);
    REQUIRE(::write(fds[1], &byte, 1) == 1);

    REQUIRE(pumpUntil(spy, 1));
    REQUIRE(spy.seen.at(0) == SIGTERM);
    ::close(fds[1]);
}

TEST_CASE("a batch coalesced into one read is reported one number at a time") {
    int fds[2] = {-1, -1};
    REQUIRE(::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);

    UnixSignalWatcher watcher(fds[0]);
    NumberSpy spy(&watcher);

    const unsigned char batch[3] = {static_cast<unsigned char>(SIGINT),
                                    static_cast<unsigned char>(SIGTERM),
                                    static_cast<unsigned char>(SIGHUP)};
    REQUIRE(::write(fds[1], batch, sizeof(batch)) == 3);

    REQUIRE(pumpUntil(spy, 3));
    REQUIRE(spy.seen == std::vector<int>{SIGINT, SIGTERM, SIGHUP});
    ::close(fds[1]);
}

TEST_CASE("an installed watcher survives the signal and reports it") {
    auto watcher = installSignalWatcher({SIGUSR1});
    REQUIRE(watcher);
    NumberSpy spy(watcher.get());

    ::raise(SIGUSR1);

    REQUIRE(pumpUntil(spy, 1));
    REQUIRE(spy.seen.at(0) == SIGUSR1);
}

TEST_CASE("a delivered signal is left on the default disposition") {
    auto watcher = installSignalWatcher({SIGUSR1, SIGUSR2});
    REQUIRE(watcher);
    REQUIRE_FALSE(dispositionIsDefault(SIGUSR1));
    NumberSpy spy(watcher.get());

    ::raise(SIGUSR1);
    REQUIRE(pumpUntil(spy, 1));

    // The second one has to kill rather than queue behind a shutdown that
    // wedged, so the handler steps aside after firing once.
    REQUIRE(dispositionIsDefault(SIGUSR1));
    REQUIRE_FALSE(dispositionIsDefault(SIGUSR2));
}

TEST_CASE("only one watcher may hold the process-wide handlers") {
    auto first = installSignalWatcher({SIGUSR1});
    REQUIRE(first);
    REQUIRE(installSignalWatcher({SIGUSR2}) == nullptr);

    first.reset();
    // Releasing has to restore the default, or a later raise would reach a
    // handler writing to a closed pipe.
    REQUIRE(dispositionIsDefault(SIGUSR1));

    auto second = installSignalWatcher({SIGUSR1});
    REQUIRE(second);
    REQUIRE_FALSE(dispositionIsDefault(SIGUSR1));
}

TEST_CASE("more numbers than the watcher can track is refused, not truncated") {
    REQUIRE(installSignalWatcher({SIGUSR1, SIGUSR2, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE,
                                  SIGALRM, SIGCHLD}) == nullptr);
    // A refusal must not leave the slot claimed.
    auto watcher = installSignalWatcher({SIGUSR1});
    REQUIRE(watcher);
}
