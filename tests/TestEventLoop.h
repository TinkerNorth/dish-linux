// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Catch2 owns no event loop, and a test that drives anything asynchronous -
// a socket, a timer, a worker's finished signal - needs one turning. These spin
// the suite's QCoreApplication, with a ceiling, so a stall fails the case
// instead of hanging the run.

#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <functional>

namespace dish::test {

// Spins until `ready` holds or `timeoutMs` passes, and answers whether it held.
inline bool spinFor(const std::function<bool()>& ready, int timeoutMs = 20000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return ready();
}

// Lets pending work land when there is no signal to wait on, which is the shape
// every "and nothing else happened" assertion needs.
inline void settle(int ms = 300) {
    spinFor([] { return false; }, ms);
}

} // namespace dish::test
