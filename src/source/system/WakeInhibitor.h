// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Holds the machine — and optionally the panel — awake while Dish streams.
// A Gateway: two D-Bus inhibits and nothing else. The lock here is "idle"/
// "block", not the "sleep"/"delay" one SleepMonitor holds: this one keeps the
// idle timer from suspending, it does not argue with a suspend the user asked
// for.

#pragma once

#include "core/reducer/KeepAwake.h"

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>

#include <optional>

namespace dish::source {

// Abstract so tests can pin the lifecycle without a live session bus.
class WakeInhibitor : public QObject {
    Q_OBJECT
  public:
    explicit WakeInhibitor(QObject* parent = nullptr) : QObject(parent) {}
    ~WakeInhibitor() override = default;

    // Idempotent and absolute: callers pass the reach they want, not a delta.
    virtual void apply(reducer::KeepAwakeReach reach, const QString& reason) = 0;
    virtual reducer::KeepAwakeReach held() const = 0;
};

// Two inhibits, because neither alone is what SetThreadExecutionState gives the
// other clients: logind's "idle" inhibit stops the idle timer suspending the
// machine mid-stream, and org.freedesktop.ScreenSaver.Inhibit stops the screen
// blanking. Releases both on destruction so a missed apply() cannot pin the
// machine awake.
class FreedesktopWakeInhibitor : public WakeInhibitor {
    Q_OBJECT
  public:
    explicit FreedesktopWakeInhibitor(QObject* parent = nullptr);
    ~FreedesktopWakeInhibitor() override;

    void apply(reducer::KeepAwakeReach reach, const QString& reason) override;
    reducer::KeepAwakeReach held() const override;

  private:
    void acquireSystem(const QString& reason);
    void releaseSystem();
    void acquireDisplay(const QString& reason);
    void releaseDisplay();

    std::optional<unsigned int> cookie_;
    // Held open for the inhibit's lifetime: logind releases when it closes.
    QDBusUnixFileDescriptor logindFd_;
};

} // namespace dish::source
