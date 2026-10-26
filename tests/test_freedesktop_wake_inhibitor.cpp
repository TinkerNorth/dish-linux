// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pins the production FreedesktopWakeInhibitor — the one holding a logind
// "idle" inhibit fd, an optional logind "handle-lid-switch" fd and an
// `org.freedesktop.ScreenSaver.Inhibit` cookie.
// test_screen_wake_controller.cpp already covers the abstract WakeInhibitor
// contract via a fake; this file exercises the concrete impl too, so the
// lifecycle isn't a "checked at runtime only" surface.
//
// Two halves. The logind locks are driven through the injected Inhibit call,
// granted with a pipe so a case can see the lock close the way logind would.
// The rest runs against the real buses, and neither logind nor a ScreenSaver
// service is guaranteed on a headless CI runner (no session bus, no DE). The
// inhibitor degrades silently when the QDBusInterface is invalid, so the
// contract verified there is only the part that does not depend on a bus
// answering: it starts at None, every call is safe, apply(None) is idempotent,
// and the destructor is safe both on a never-applied and on an applied
// instance.

#include "source/system/WakeInhibitor.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDBusUnixFileDescriptor>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <map>
#include <poll.h>
#include <unistd.h>
#include <vector>

using dish::reducer::KeepAwakeHold;
using dish::reducer::KeepAwakeReach;
using dish::source::FreedesktopWakeInhibitor;
using dish::source::LogindInhibit;

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
        static auto* const app = new QCoreApplication(argc, argv);
        REQUIRE(QCoreApplication::instance() == app);
    }
};

constexpr KeepAwakeHold kMachine{KeepAwakeReach::System, false};
constexpr KeepAwakeHold kMachineAndLid{KeepAwakeReach::System, true};
constexpr KeepAwakeHold kNothing{};

// logind's two lock names, as Manager.Inhibit spells them.
QString idleLock() { return QStringLiteral("idle"); }
QString lidLock() { return QStringLiteral("handle-lid-switch"); }

// Grants each Inhibit with the write end of a fresh pipe and keeps the read
// end. logind releases a lock when its fd closes; here the read end sees EOF
// once the inhibitor has closed its copy, so "held" is observed, not assumed.
class FakeLogind {
  public:
    std::vector<QString> refuse;

    FakeLogind() = default;
    FakeLogind(const FakeLogind&) = delete;
    FakeLogind& operator=(const FakeLogind&) = delete;
    ~FakeLogind() {
        for (const auto& entry : readEnds_) { ::close(entry.second); }
    }

    LogindInhibit gateway() {
        return [this](const QString& what, const QString& reason) { return grant(what, reason); };
    }

    QDBusUnixFileDescriptor grant(const QString& what, const QString& reason) {
        asked_.push_back(what);
        lastReason_ = reason;
        const bool refused = std::find(refuse.begin(), refuse.end(), what) != refuse.end();
        if (refused) { return QDBusUnixFileDescriptor(); }
        int ends[2] = {-1, -1};
        REQUIRE(::pipe(ends) == 0);
        // The descriptor dups the write end, so ours closes at once and the
        // inhibitor's copy is the only one left.
        const QDBusUnixFileDescriptor lock(ends[1]);
        ::close(ends[1]);
        forget(what);
        readEnds_[what] = ends[0];
        return lock;
    }

    bool isHeld(const QString& what) const {
        const auto found = readEnds_.find(what);
        if (found == readEnds_.end()) { return false; }
        pollfd readEnd{found->second, POLLIN, 0};
        const int ready = ::poll(&readEnd, 1, 0);
        const bool nothingToReadYet = ready == 0;
        return nothingToReadYet;
    }

    std::ptrdiff_t timesAsked(const QString& what) const {
        return std::count(asked_.begin(), asked_.end(), what);
    }
    QString lastReason() const { return lastReason_; }

  private:
    void forget(const QString& what) {
        const auto found = readEnds_.find(what);
        if (found == readEnds_.end()) { return; }
        ::close(found->second);
        readEnds_.erase(found);
    }

    std::vector<QString> asked_;
    std::map<QString, int> readEnds_;
    QString lastReason_;
};

} // namespace

TEST_CASE("Freedesktop wake inhibitor: the machine alone takes only the idle lock",
          "[wake][linux][lid]") {
    FakeLogind logind;
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachine, QStringLiteral("streaming"));
    REQUIRE(logind.timesAsked(idleLock()) == 1);
    REQUIRE(logind.timesAsked(lidLock()) == 0);
    REQUIRE(logind.isHeld(idleLock()));
    REQUIRE(inh.held() == kMachine);
}

TEST_CASE("Freedesktop wake inhibitor: the lid takes a second, handle-lid-switch lock",
          "[wake][linux][lid]") {
    FakeLogind logind;
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming with the lid shut"));
    REQUIRE(logind.isHeld(idleLock()));
    REQUIRE(logind.isHeld(lidLock()));
    REQUIRE(logind.lastReason() == QStringLiteral("streaming with the lid shut"));
    REQUIRE(inh.held() == kMachineAndLid);
}

TEST_CASE("Freedesktop wake inhibitor: dropping the lid releases its lock and keeps the idle one",
          "[wake][linux][lid]") {
    FakeLogind logind;
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    inh.apply(kMachine, QStringLiteral("streaming"));
    REQUIRE_FALSE(logind.isHeld(lidLock()));
    REQUIRE(logind.isHeld(idleLock()));
    REQUIRE(logind.timesAsked(idleLock()) == 1);
    REQUIRE(inh.held() == kMachine);
}

TEST_CASE("Freedesktop wake inhibitor: apply(None) releases the lid with the machine",
          "[wake][linux][lid]") {
    FakeLogind logind;
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    inh.apply(kNothing, QStringLiteral("streaming"));
    REQUIRE_FALSE(logind.isHeld(lidLock()));
    REQUIRE_FALSE(logind.isHeld(idleLock()));
    REQUIRE(inh.held() == kNothing);
}

TEST_CASE("Freedesktop wake inhibitor: destruction releases the lid lock", "[wake][linux][lid]") {
    FakeLogind logind;
    {
        FreedesktopWakeInhibitor inh(logind.gateway());
        inh.apply(kMachineAndLid, QStringLiteral("dies on scope exit"));
        REQUIRE(logind.isHeld(lidLock()));
    }
    REQUIRE_FALSE(logind.isHeld(lidLock()));
    REQUIRE_FALSE(logind.isHeld(idleLock()));
}

TEST_CASE("Freedesktop wake inhibitor: a repeated lid apply does not stack a second lock",
          "[wake][linux][lid]") {
    FakeLogind logind;
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    REQUIRE(logind.timesAsked(lidLock()) == 1);
    REQUIRE(logind.isHeld(lidLock()));
}

TEST_CASE("Freedesktop wake inhibitor: a refused lid lock is reported as not held",
          "[wake][linux][lid]") {
    FakeLogind logind;
    logind.refuse.push_back(lidLock());
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    // Not fatal: the machine is still held, only the lid is missing.
    REQUIRE(logind.isHeld(idleLock()));
    REQUIRE(inh.held() == kMachine);
}

TEST_CASE("Freedesktop wake inhibitor: a refused lid lock is asked for again on the next apply",
          "[wake][linux][lid]") {
    FakeLogind logind;
    logind.refuse.push_back(lidLock());
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    logind.refuse.clear();
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    REQUIRE(logind.timesAsked(lidLock()) == 2);
    REQUIRE(inh.held() == kMachineAndLid);
}

TEST_CASE("Freedesktop wake inhibitor: a refused idle lock under a granted lid says exactly that",
          "[wake][linux][lid]") {
    FakeLogind logind;
    logind.refuse.push_back(idleLock());
    FreedesktopWakeInhibitor inh(logind.gateway());
    inh.apply(kMachineAndLid, QStringLiteral("streaming"));
    REQUIRE(inh.held() == KeepAwakeHold{KeepAwakeReach::None, true});
}

TEST_CASE("Freedesktop wake inhibitor starts holding nothing", "[wake][linux]") {
    QtAppSingleton::ensure();
    const FreedesktopWakeInhibitor inh;
    REQUIRE(inh.held() == kNothing);
}

TEST_CASE("Freedesktop wake inhibitor never crashes regardless of bus availability",
          "[wake][linux]") {
    QtAppSingleton::ensure();
    // The contract: regardless of whether logind and the ScreenSaver service
    // are reachable, every call must be safe and the state self-consistent. On
    // a real desktop held() reports what was actually acquired; on CI without a
    // bus it stays None — both branches are correct.
    FreedesktopWakeInhibitor inh;
    REQUIRE(inh.held() == kNothing);

    inh.apply(kMachine, QStringLiteral("test reason"));
    // held() is deliberately not asserted here — too environment-dependent.
    // What matters is that releasing is safe whether or not acquiring worked.
    inh.apply(kNothing, QStringLiteral("test reason"));
    REQUIRE(inh.held() == kNothing);

    inh.apply(KeepAwakeHold{KeepAwakeReach::SystemAndDisplay, false}, QStringLiteral("wider"));
    inh.apply(kNothing, QStringLiteral("wider"));
    REQUIRE(inh.held() == kNothing);
}

TEST_CASE("Freedesktop wake inhibitor: apply(None) is idempotent", "[wake][linux]") {
    QtAppSingleton::ensure();
    FreedesktopWakeInhibitor inh;
    // A release on a never-acquired inhibitor must not send an UnInhibit for a
    // cookie that does not exist, however many times it is asked.
    inh.apply(kNothing, QStringLiteral("nothing to release"));
    inh.apply(kNothing, QStringLiteral("nothing to release"));
    inh.apply(kNothing, QStringLiteral("nothing to release"));
    REQUIRE(inh.held() == kNothing);
}

TEST_CASE("Freedesktop wake inhibitor: repeated apply of the same reach is safe", "[wake][linux]") {
    QtAppSingleton::ensure();
    FreedesktopWakeInhibitor inh;
    // The controller calls apply() on every emission, so a re-apply of a reach
    // already held must not stack a second inhibit.
    inh.apply(kMachine, QStringLiteral("streaming"));
    inh.apply(kMachine, QStringLiteral("streaming"));
    inh.apply(kNothing, QStringLiteral("streaming"));
    REQUIRE(inh.held() == kNothing);
}

TEST_CASE("Freedesktop wake inhibitor: dtor on a never-applied instance is a no-op",
          "[wake][linux]") {
    QtAppSingleton::ensure();
    {
        const FreedesktopWakeInhibitor inh;
        REQUIRE(inh.held() == kNothing);
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
        inh.apply(KeepAwakeHold{KeepAwakeReach::SystemAndDisplay, false},
                  QStringLiteral("dies on scope exit"));
    }
    SUCCEED();
}
