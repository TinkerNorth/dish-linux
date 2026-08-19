// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

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

// org.freedesktop.ScreenSaver.Inhibit on the session bus, which every modern
// desktop honours. Releases on destruction so a missed release() can't pin the
// display awake.
class FreedesktopScreenSaverInhibitor : public DisplaySleepInhibitor {
    Q_OBJECT
  public:
    explicit FreedesktopScreenSaverInhibitor(QObject* parent = nullptr);
    ~FreedesktopScreenSaverInhibitor() override;

    void acquire(const QString& reason) override;
    void release() override;
    bool isHeld() const override { return cookie_.has_value(); }

  private:
    std::optional<unsigned int> cookie_;
};

} // namespace dish::util
