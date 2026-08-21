// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// logind's PrepareForSleep edge, and the delay lock that buys time on it.
// Abstract so tests can drive the edge without a system bus.
//
// The lock is "sleep"/"delay", not the "idle"/"block" one DisplaySleepInhibitor
// holds: this one does not refuse a suspend, it only asks logind to wait while
// Dish closes its sessions. Held continuously and dropped inside the handler,
// because logind suspends the moment the last delay lock closes.

#pragma once

#include <QDBusUnixFileDescriptor>
#include <QObject>

namespace dish::source {

class SleepMonitor : public QObject {
    Q_OBJECT
  public:
    explicit SleepMonitor(QObject* parent = nullptr) : QObject(parent) {}
    ~SleepMonitor() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;

  signals:
    void preparingForSleep(bool starting);
};

class LogindSleepMonitor : public SleepMonitor {
    Q_OBJECT
  public:
    explicit LogindSleepMonitor(QObject* parent = nullptr);
    ~LogindSleepMonitor() override;

    void start() override;
    void stop() override;

    bool isDelayHeld() const { return delayFd_.isValid(); }

  private slots:
    void handlePrepareForSleep(bool starting);

  private:
    void acquireDelay();
    void releaseDelay();

    QDBusUnixFileDescriptor delayFd_;
    bool started_ = false;
};

} // namespace dish::source
