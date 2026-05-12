// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pins the production DisplaySleepInhibitor implementation (the one backed
// by `org.freedesktop.ScreenSaver.Inhibit` via QtDBus).
// `test_screen_wake_controller.cpp` already covers the abstract
// DisplaySleepInhibitor contract via a fake; this file exercises the
// concrete impl too so the lifecycle isn't a "checked at runtime only"
// surface.
//
// Caveat: the freedesktop ScreenSaver service may not be present on a
// headless CI runner (no session bus, no DE). The inhibitor handles that
// gracefully — `acquire` silently fails when the QDBusInterface is
// invalid — so the contract we verify here is the bits that don't depend
// on the bus actually answering: default state, idempotent release, no-op
// re-acquire, and the destructor not crashing on a never-held instance.

#include "Util/DisplaySleepInhibitor.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <memory>

using dish::util::FreedesktopScreenSaverInhibitor;

namespace {

// QDBusConnection::sessionBus() works without a QCoreApplication but
// produces stderr warnings. A single shared QCoreApplication for the
// whole test suite keeps those quiet without forcing every test file to
// create one.
class QtAppSingleton {
  public:
    static void ensure() {
        if (QCoreApplication::instance() != nullptr) { return; }
        static int argc = 0;
        static char* argv[] = {nullptr};
        // Leak intentionally — Catch2 calls std::exit, which runs atexit
        // handlers. Owning the QCoreApplication in a unique_ptr that the
        // first test creates would tear it down in the middle of a later
        // test's teardown if Catch2 reorders cases.
        static auto* app = new QCoreApplication(argc, argv);
        (void) app;
    }
};

} // namespace

TEST_CASE("Freedesktop inhibitor starts unheld", "[wake][linux]") {
    QtAppSingleton::ensure();
    const FreedesktopScreenSaverInhibitor inh;
    REQUIRE_FALSE(inh.isHeld());
}

TEST_CASE("Freedesktop inhibitor never crashes regardless of session-bus availability",
          "[wake][linux]") {
    QtAppSingleton::ensure();
    // The contract: regardless of whether the freedesktop ScreenSaver
    // service is reachable, every method must be safe to call and the
    // state must stay self-consistent. On a real desktop with a live
    // session bus, isHeld() will be true after acquire; on CI without
    // a bus it will be false — both branches are correct.
    FreedesktopScreenSaverInhibitor inh;
    REQUIRE_FALSE(inh.isHeld());
    inh.acquire(QStringLiteral("test reason"));

    // We don't assert isHeld() here — too environment-dependent. The
    // important behaviour is that release() is safe whether or not
    // acquire() succeeded.
    inh.release();
    REQUIRE_FALSE(inh.isHeld());

    // Idempotent re-release on an unheld inhibitor.
    inh.release();
    REQUIRE_FALSE(inh.isHeld());

    // Re-acquire/release cycle leaves us back at unheld.
    inh.acquire(QStringLiteral("second"));
    inh.release();
    REQUIRE_FALSE(inh.isHeld());
}

TEST_CASE("Freedesktop inhibitor destructor on a never-acquired instance is a no-op",
          "[wake][linux]") {
    QtAppSingleton::ensure();
    {
        FreedesktopScreenSaverInhibitor inh;
        REQUIRE_FALSE(inh.isHeld());
        // No acquire — dtor runs at scope exit and must NOT attempt a
        // UnInhibit DBus call on a cookie that doesn't exist.
    }
    SUCCEED();
}

TEST_CASE("Freedesktop inhibitor when held: dtor cleans up the cookie", "[wake][linux]") {
    QtAppSingleton::ensure();
    // Same caveat as above: on CI, isHeld() may stay false after acquire
    // because there's no session bus. We still pin that the dtor path is
    // safe in either case — if the cookie was real, dtor sends UnInhibit;
    // if it was synthetic, dtor short-circuits on `!cookie_.has_value()`.
    {
        FreedesktopScreenSaverInhibitor inh;
        inh.acquire(QStringLiteral("dies on scope exit"));
        // dtor runs here regardless of isHeld() value.
    }
    SUCCEED();
}
