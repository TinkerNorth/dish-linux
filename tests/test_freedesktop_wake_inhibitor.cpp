// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pins the production FreedesktopWakeInhibitor — the one holding a logind
// "idle" inhibit fd and an `org.freedesktop.ScreenSaver.Inhibit` cookie.
// test_screen_wake_controller.cpp already covers the abstract WakeInhibitor
// contract via a fake; this file exercises the concrete impl too, so the
// lifecycle isn't a "checked at runtime only" surface.
//
// Caveat: neither logind nor a ScreenSaver service is guaranteed on a headless
// CI runner (no session bus, no DE). The inhibitor degrades silently when the
// QDBusInterface is invalid, so the contract verified here is only the part
// that does not depend on a bus answering: it starts at None, every call is
// safe, apply(None) is idempotent, and the destructor is safe both on a
// never-applied and on an applied instance.

#include "source/system/WakeInhibitor.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

using dish::reducer::KeepAwakeReach;
using dish::source::FreedesktopWakeInhibitor;

namespace {

// QDBusConnection::sessionBus() works without a QCoreApplication but produces
// stderr warnings. A single shared QCoreApplication for the whole test suite
// keeps those quiet without forcing every test file to create one.
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
        (void)app;
    }
};

} // namespace

TEST_CASE("Freedesktop wake inhibitor starts holding nothing", "[wake][linux]") {
    QtAppSingleton::ensure();
    const FreedesktopWakeInhibitor inh;
    REQUIRE(inh.held() == KeepAwakeReach::None);
}

TEST_CASE("Freedesktop wake inhibitor never crashes regardless of bus availability",
          "[wake][linux]") {
    QtAppSingleton::ensure();
    // The contract: regardless of whether logind and the ScreenSaver service
    // are reachable, every call must be safe and the state self-consistent. On
    // a real desktop held() reports what was actually acquired; on CI without a
    // bus it stays None — both branches are correct.
    FreedesktopWakeInhibitor inh;
    REQUIRE(inh.held() == KeepAwakeReach::None);

    inh.apply(KeepAwakeReach::System, QStringLiteral("test reason"));
    // held() is deliberately not asserted here — too environment-dependent.
    // What matters is that releasing is safe whether or not acquiring worked.
    inh.apply(KeepAwakeReach::None, QStringLiteral("test reason"));
    REQUIRE(inh.held() == KeepAwakeReach::None);

    inh.apply(KeepAwakeReach::SystemAndDisplay, QStringLiteral("wider"));
    inh.apply(KeepAwakeReach::None, QStringLiteral("wider"));
    REQUIRE(inh.held() == KeepAwakeReach::None);
}

TEST_CASE("Freedesktop wake inhibitor: apply(None) is idempotent", "[wake][linux]") {
    QtAppSingleton::ensure();
    FreedesktopWakeInhibitor inh;
    // A release on a never-acquired inhibitor must not send an UnInhibit for a
    // cookie that does not exist, however many times it is asked.
    inh.apply(KeepAwakeReach::None, QStringLiteral("nothing to release"));
    inh.apply(KeepAwakeReach::None, QStringLiteral("nothing to release"));
    inh.apply(KeepAwakeReach::None, QStringLiteral("nothing to release"));
    REQUIRE(inh.held() == KeepAwakeReach::None);
}

TEST_CASE("Freedesktop wake inhibitor: repeated apply of the same reach is safe", "[wake][linux]") {
    QtAppSingleton::ensure();
    FreedesktopWakeInhibitor inh;
    // The controller calls apply() on every emission, so a re-apply of a reach
    // already held must not stack a second inhibit.
    inh.apply(KeepAwakeReach::System, QStringLiteral("streaming"));
    inh.apply(KeepAwakeReach::System, QStringLiteral("streaming"));
    inh.apply(KeepAwakeReach::None, QStringLiteral("streaming"));
    REQUIRE(inh.held() == KeepAwakeReach::None);
}

TEST_CASE("Freedesktop wake inhibitor: dtor on a never-applied instance is a no-op",
          "[wake][linux]") {
    QtAppSingleton::ensure();
    {
        const FreedesktopWakeInhibitor inh;
        REQUIRE(inh.held() == KeepAwakeReach::None);
        // No apply — the dtor runs at scope exit and must NOT attempt an
        // UnInhibit on a cookie that was never issued.
    }
    SUCCEED();
}

TEST_CASE("Freedesktop wake inhibitor: dtor cleans up after an apply", "[wake][linux]") {
    QtAppSingleton::ensure();
    // Same caveat as above: on CI held() may stay None after apply because
    // there is no bus. The dtor path is pinned safe either way — a real cookie
    // gets an UnInhibit, an absent one short-circuits.
    {
        FreedesktopWakeInhibitor inh;
        inh.apply(KeepAwakeReach::SystemAndDisplay, QStringLiteral("dies on scope exit"));
    }
    SUCCEED();
}
