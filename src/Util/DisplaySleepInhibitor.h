// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>

#include <optional>

namespace dish::util {

// Keeps the display and the system awake while streaming. Abstract so tests
// can pin the acquire/release lifecycle without a live session bus.
class DisplaySleepInhibitor : public QObject {
    Q_OBJECT
  public:
    explicit DisplaySleepInhibitor(QObject* parent = nullptr) : QObject(parent) {}
    ~DisplaySleepInhibitor() override = default;

    // Both idempotent: callers need not track held state themselves.
    virtual void acquire(const QString& reason) = 0;
    virtual void release() = 0;
    virtual bool isHeld() const = 0;
};

// Two inhibits, because neither alone is what SetThreadExecutionState gives
// the other clients: org.freedesktop.ScreenSaver.Inhibit (session bus) stops
// the screen blanking, and logind's "idle" inhibit stops the idle timer
// suspending the machine mid-stream. Releases both on destruction so a missed
// release() can't pin the display awake.
class FreedesktopScreenSaverInhibitor : public DisplaySleepInhibitor {
    Q_OBJECT
  public:
    explicit FreedesktopScreenSaverInhibitor(QObject* parent = nullptr);
    ~FreedesktopScreenSaverInhibitor() override;

    void acquire(const QString& reason) override;
    void release() override;
    bool isHeld() const override { return cookie_.has_value() || logindFd_.isValid(); }

  private:
    void acquireLogindIdle(const QString& reason);
    void releaseLogindIdle();

    std::optional<unsigned int> cookie_;
    // Held open for the inhibit's lifetime: logind releases when it closes.
    QDBusUnixFileDescriptor logindFd_;
};

} // namespace dish::util
