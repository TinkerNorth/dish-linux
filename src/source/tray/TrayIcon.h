// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The tray item, as the rest of Dish sees it. Abstract so the close policy and
// its controller can be pinned without a session bus or a StatusNotifier host.
//
// isAvailable() is load-bearing rather than cosmetic: it gates whether closing
// the window is allowed to hide it, and a wrong answer strands a running Dish
// with no window to return to and no menu to quit from.

#pragma once

#include "core/reducer/TrayPresentation.h"

#include <QObject>

namespace dish::source {

class TrayIcon : public QObject {
    Q_OBJECT
  public:
    explicit TrayIcon(QObject* parent = nullptr) : QObject(parent) {}
    ~TrayIcon() override = default;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool isAvailable() const = 0;
    virtual void setPresentation(const reducer::TrayPresentation& presentation) = 0;

  signals:
    void showWindowRequested();
    void quitRequested();
    void availabilityChanged(bool available);
};

} // namespace dish::source
