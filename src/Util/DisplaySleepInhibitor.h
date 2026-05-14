// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QObject>
#include <QString>

#include <optional>

namespace dish::util {

// Keeps the system display awake while Dish is streaming. The Linux analogue
// of Android's WakeStateController (PARTIAL_WAKE_LOCK + FLAG_KEEP_SCREEN_ON)
// and dish-mac's IOPMAssertionCreateWithName. Implemented over D-Bus by
// calling org.freedesktop.ScreenSaver.Inhibit on the session bus — the
// portal every modern desktop environment honours (GNOME, KDE, Xfce, Cinnamon,
// MATE, Sway/swayidle, …).
//
// Tests use FakeDisplaySleepInhibitor so we can pin the acquire/release
// lifecycle without a session-bus dependency in CI.
class DisplaySleepInhibitor : public QObject {
    Q_OBJECT
  public:
    explicit DisplaySleepInhibitor(QObject* parent = nullptr) : QObject(parent) {}
    ~DisplaySleepInhibitor() override = default;

    // Idempotent: a second acquire while already held is a no-op so callers
    // don't have to track state themselves.
    virtual void acquire(const QString& reason) = 0;
    // Idempotent: releasing while not held is a no-op.
    virtual void release() = 0;
    // True iff an inhibit cookie is currently held.
    virtual bool isHeld() const = 0;
};

// Production implementation. Held in a dedicated class so the cookie lifetime
// is tied to the object's lifetime — destructor releases on dealloc.
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
