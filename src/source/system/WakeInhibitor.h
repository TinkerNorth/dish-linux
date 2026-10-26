// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Holds the machine — and optionally the panel and the lid — awake while Dish
// streams. A Gateway: D-Bus inhibits and nothing else. The locks here are
// "block" ones, not the "sleep"/"delay" one SleepMonitor holds: "idle" keeps the
// idle timer from suspending, and "handle-lid-switch", only when the user opted
// in, keeps closing the lid from suspending. Neither argues with a suspend the
// user asked for from a menu.

#pragma once

#include "core/reducer/KeepAwake.h"

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace dish::source {

// Abstract so tests can pin the lifecycle without a live session bus.
class WakeInhibitor : public QObject {
    Q_OBJECT
  public:
    explicit WakeInhibitor(QObject* parent = nullptr) : QObject(parent) {}
    ~WakeInhibitor() override = default;

    // Idempotent and absolute: callers pass the hold they want, not a delta.
    virtual void apply(const reducer::KeepAwakeHold& hold, const QString& reason) = 0;
    // What was actually achieved, which a refused lock makes less than asked.
    virtual reducer::KeepAwakeHold held() const = 0;
};

// One logind Manager.Inhibit(what, "Dish", reason, "block") call: the lock's
// fd, or an invalid one when logind is absent or refuses.
using LogindInhibit =
    std::function<QDBusUnixFileDescriptor(const QString& what, const QString& reason)>;

// Up to three inhibits, because neither of the first two alone is what
// SetThreadExecutionState gives the other clients: logind's "idle" inhibit
// stops the idle timer suspending the machine mid-stream, and
// org.freedesktop.ScreenSaver.Inhibit stops the screen blanking. The third,
// logind's "handle-lid-switch", is the lid opt-in. Releases all of them on
// destruction so a missed apply() cannot pin the machine awake.
class FreedesktopWakeInhibitor : public WakeInhibitor {
    Q_OBJECT
  public:
    // Takes the logind locks over the system bus.
    explicit FreedesktopWakeInhibitor(QObject* parent = nullptr);
    // Takes the logind locks through `inhibit`, so a test can grant or refuse
    // them without a system bus.
    explicit FreedesktopWakeInhibitor(LogindInhibit inhibit, QObject* parent = nullptr);
    ~FreedesktopWakeInhibitor() override;

    void apply(const reducer::KeepAwakeHold& hold, const QString& reason) override;
    reducer::KeepAwakeHold held() const override;

  private:
    reducer::KeepAwakeReach heldReach() const;
    void acquireSystem(const QString& reason);
    void releaseSystem();
    void acquireDisplay(const QString& reason);
    void releaseDisplay();
    void acquireLid(const QString& reason);
    void releaseLid();

    LogindInhibit inhibit_;
    std::optional<unsigned int> cookie_;
    // Each held open for its inhibit's lifetime: logind releases when it closes.
    QDBusUnixFileDescriptor logindFd_;
    QDBusUnixFileDescriptor lidFd_;
};

} // namespace dish::source
